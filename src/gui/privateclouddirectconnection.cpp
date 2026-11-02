/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifdef _WIN32
#include <qt_windows.h>
#include <winsvc.h>
#endif

#include "privateclouddirectconnection.h"
#include "privatecloudtransport.h"

#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QUuid>
#include <QtConcurrentRun>
#include <cmath>
#include <limits>
#include <memory>

namespace OCC
{
Q_LOGGING_CATEGORY(lcPrivateCloudDirect, "nextcloud.gui.privatecloud.direct", QtInfoMsg)

namespace
{
constexpr int replyLimit = 4096;
constexpr int responseTimeoutMs = 10000;
constexpr int renewalIntervalMs = 60000;
constexpr int leaseDurationMs = 120000;
constexpr int initialRetryMs = 1000;
constexpr int maximumRetryMs = 30000;

// The result owns its HANDLE even if the watcher is canceled or destroyed before
// the worker completes. Adoption sets its value to zero to transfer ownership.
std::shared_ptr<qintptr> openTrustedPipe()
{
#ifdef Q_OS_WIN
    const auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return {};
    }
    const auto closeScm = qScopeGuard([scm] {
        CloseServiceHandle(scm);
    });
    const auto service = OpenServiceW(scm, L"privateCloudDirectBroker", SERVICE_QUERY_STATUS);
    if (!service) {
        return {};
    }
    const auto closeService = qScopeGuard([service] {
        CloseServiceHandle(service);
    });
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)
        || status.dwCurrentState != SERVICE_RUNNING || !status.dwProcessId || !(status.dwServiceType & SERVICE_WIN32_OWN_PROCESS)) {
        return {};
    }
    const auto expectedPid = status.dwProcessId;
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, expectedPid);
    if (!process) {
        return {};
    }
    const auto closeProcess = qScopeGuard([process] {
        CloseHandle(process);
    });
    const auto pipe = CreateFileW(L"\\\\.\\pipe\\privateCloudDirectBroker-v1",
                                  FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                                  0,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                  nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return {};
    }
    auto handle = std::shared_ptr<qintptr>(new qintptr(reinterpret_cast<qintptr>(pipe)), [](qintptr *value) {
        if (*value) {
            CloseHandle(reinterpret_cast<HANDLE>(*value));
        }
        delete value;
    });
    ULONG serverPid = 0;
    if (GetFileType(pipe) != FILE_TYPE_PIPE || !GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid != expectedPid
        || WaitForSingleObject(process, 0) != WAIT_TIMEOUT
        || !QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)
        || status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId != serverPid) {
        return {};
    }
    return handle;
#else
    return {};
#endif
}

bool integer(const QJsonValue &value, double minimum, double maximum)
{
    return value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum && std::floor(value.toDouble()) == value.toDouble();
}

bool publicIpv4(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    const auto ip = address.toIPv4Address();
    const auto a = ip >> 24;
    const auto b = (ip >> 16) & 255;
    const auto c = (ip >> 8) & 255;
    // Match the broker's public IPv4 allow policy, including documentation and
    // benchmarking ranges that must never be used as a remote endpoint.
    return a != 0 && a != 10 && a != 127 && a < 224 && !(a == 100 && b >= 64 && b <= 127) && !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31)
        && !(a == 192 && b == 168) && !(a == 192 && b == 0 && (c == 0 || c == 2)) && !(a == 192 && b == 88 && c == 99) && !(a == 198 && (b == 18 || b == 19))
        && !(a == 198 && b == 51 && c == 100) && !(a == 203 && b == 0 && c == 113);
}
}

PrivateCloudDirectConnection::PrivateCloudDirectConnection(QObject *parent)
    : QObject(parent)
{
    _clock.start();
    _now = [this] {
        return _clock.elapsed();
    };
    _openPipe = [this](quint64 generation) {
        openBrokerPipe(generation);
    };
    for (const auto timer : {&_retryTimer, &_responseTimer, &_renewalTimer, &_expiryTimer}) {
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
    }
    connect(&_retryTimer, &QTimer::timeout, this, &PrivateCloudDirectConnection::attemptConnection);
    connect(&_responseTimer, &QTimer::timeout, this, [this] {
        fail(tr("The privateCloud direct connection broker did not respond in time."));
    });
    connect(&_renewalTimer, &QTimer::timeout, this, &PrivateCloudDirectConnection::renewLease);
    connect(&_expiryTimer, &QTimer::timeout, this, &PrivateCloudDirectConnection::expireLease);
}

PrivateCloudDirectConnection::~PrivateCloudDirectConnection()
{
    stop();
}

void PrivateCloudDirectConnection::start()
{
    if (_active) {
        return;
    }
    _active = true;
    _retryDelayMs = initialRetryMs;
    PrivateCloudTransport::enableRequired();
    PrivateCloudTransport::clearEndpoint();
    _retryTimer.start(0);
}

void PrivateCloudDirectConnection::stop()
{
    if (!_active) {
        return;
    }
    _active = false;
    _retryTimer.stop();
    closeSession();
}

bool PrivateCloudDirectConnection::isReady() const
{
    return _ready;
}

QString PrivateCloudDirectConnection::lastError() const
{
    return _lastError;
}

void PrivateCloudDirectConnection::attemptConnection()
{
    if (!_active) {
        return;
    }
    // Do not create an unbounded series of workers if a native service query is
    // slow. Its stale result still has an owner and will close its own HANDLE.
    if (_openWatcher) {
        _retryTimer.start(_retryDelayMs);
        return;
    }
    _responseTimer.start(responseTimeoutMs);
    _openPipe(++_generation);
}

void PrivateCloudDirectConnection::openBrokerPipe(quint64 generation)
{
    auto *watcher = new QFutureWatcher<std::shared_ptr<qintptr>>(this);
    _openWatcher = watcher;
    connect(watcher, &QFutureWatcher<std::shared_ptr<qintptr>>::finished, this, [this, watcher, generation] {
        const auto handle = watcher->result();
        _openWatcher.clear();
        watcher->deleteLater();
        if (!_active || generation != _generation) {
            return;
        }
        if (!handle || !*handle) {
            fail(tr("The privateCloud connection helper is unavailable or could not be verified. Install or repair privateCloud with administrator approval."));
            return;
        }
        auto *socket = new QLocalSocket(this);
        if (!socket->setSocketDescriptor(*handle, QLocalSocket::ConnectedState, QIODevice::ReadWrite)) {
            socket->deleteLater();
            fail(tr("Could not open the privateCloud direct connection broker channel."));
            return;
        }
        *handle = 0;
        adoptPipe(socket, generation);
    });
    watcher->setFuture(QtConcurrent::run(openTrustedPipe));
}

void PrivateCloudDirectConnection::adoptPipe(QLocalSocket *socket, quint64 generation)
{
    if (!_active || generation != _generation || _pipe) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    _pipe = socket;
    socket->setParent(this);
    socket->setReadBufferSize(replyLimit + 1);
    connect(socket, &QLocalSocket::readyRead, this, &PrivateCloudDirectConnection::readReply);
    const auto disconnected = [this, socket, generation] {
        if (_pipe == socket && generation == _generation) {
            fail(tr("The privateCloud direct connection lease was revoked or its broker disconnected."));
        }
    };
    connect(socket, &QLocalSocket::disconnected, this, disconnected);
    connect(socket, &QLocalSocket::errorOccurred, this, disconnected);
    sendRequest(false);
}

void PrivateCloudDirectConnection::sendRequest(bool renewal)
{
    if (!_pipe || _awaitingReply || (renewal && _now() >= _expiresAt)) {
        fail(tr("The privateCloud direct connection lease expired."));
        return;
    }
    _awaitingReply = true;
    _requestAt = _now();
    _reply.clear();
    _responseTimer.start(responseTimeoutMs);
    const auto request = renewal ? QByteArrayLiteral("PRIVATECLOUD-RENEW/1\n") : QByteArrayLiteral("PRIVATECLOUD-DIRECT/1\n");
    if (_pipe->write(request) != request.size()) {
        fail(tr("Could not request a privateCloud direct connection lease."));
    }
}

void PrivateCloudDirectConnection::readReply()
{
    if (!_pipe || !_awaitingReply) {
        fail(tr("The privateCloud direct connection broker sent an unexpected response."));
        return;
    }
    _reply += _pipe->read(replyLimit + 1 - _reply.size());
    const auto newline = _reply.indexOf('\n');
    if (_reply.size() > replyLimit || _reply.contains('\\') || (newline >= 0 && newline != _reply.size() - 1)) {
        fail(tr("The privateCloud direct connection broker sent an invalid response."));
        return;
    }
    if (newline < 0) {
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(_reply, &error);
    const auto object = document.object();
    // The protocol has only unescaped ASCII keys and scalar values. Reject
    // escapes above before counting keys, including escaped duplicate names
    // that QJsonDocument would otherwise silently merge.
    static const QRegularExpression member(QStringLiteral("\"[A-Za-z][A-Za-z0-9]*\"\\s*:"));
    auto matches = member.globalMatch(QString::fromUtf8(_reply));
    auto members = 0;
    while (matches.hasNext()) {
        matches.next();
        ++members;
    }
    if (error.error != QJsonParseError::NoError || !document.isObject() || members != object.size() || !integer(object.value(QStringLiteral("version")), 1, 1)
        || !object.value(QStringLiteral("ok")).isBool()) {
        fail(tr("The privateCloud direct connection broker sent an invalid response."));
        return;
    }
    if (!object.value(QStringLiteral("ok")).toBool()) {
        static const QRegularExpression errorCode(QStringLiteral("^[a-z0-9_]{1,64}$"));
        const auto code = object.value(QStringLiteral("error")).toString();
        if (object.size() != 3 || !errorCode.match(code).hasMatch()) {
            fail(tr("The privateCloud direct connection broker sent an invalid response."));
        } else {
            //: %1 is a short machine-readable error code from the local broker.
            fail(tr("The privateCloud direct connection broker refused the lease (%1).").arg(code));
        }
        return;
    }
    if (!validLease(object)) {
        fail(tr("The privateCloud direct connection broker returned an invalid lease."));
        return;
    }
    acceptReply(object);
}

bool PrivateCloudDirectConnection::validLease(const QJsonObject &reply)
{
    static const auto keys = QSet<QString>{QStringLiteral("version"),
                                           QStringLiteral("ok"),
                                           QStringLiteral("lease"),
                                           QStringLiteral("family"),
                                           QStringLiteral("server"),
                                           QStringLiteral("port"),
                                           QStringLiteral("interfaceIndex"),
                                           QStringLiteral("source"),
                                           QStringLiteral("expiresInSeconds"),
                                           QStringLiteral("permitInstalled"),
                                           QStringLiteral("renewed")};
    const auto actualKeys = reply.keys();
    const auto serverText = reply.value(QStringLiteral("server")).toString();
    const auto sourceText = reply.value(QStringLiteral("source")).toString();
    const auto server = QHostAddress(serverText);
    const auto source = QHostAddress(sourceText);
    const auto sourceFirstOctet = source.toIPv4Address() >> 24;
    const auto leaseText = reply.value(QStringLiteral("lease")).toString();
    const auto id = QUuid::fromString(leaseText);
    return QSet<QString>(actualKeys.begin(), actualKeys.end()) == keys && integer(reply.value(QStringLiteral("family")), 4, 4)
        && integer(reply.value(QStringLiteral("port")), 443, 443) && integer(reply.value(QStringLiteral("expiresInSeconds")), 120, 120)
        && integer(reply.value(QStringLiteral("interfaceIndex")), 1, std::numeric_limits<quint32>::max())
        && reply.value(QStringLiteral("permitInstalled")).isBool() && reply.value(QStringLiteral("renewed")).isBool() && !id.isNull()
        && id.toString(QUuid::WithoutBraces) == leaseText.toLower() && publicIpv4(server) && server.toString() == serverText
        && source.protocol() == QAbstractSocket::IPv4Protocol && source.toString() == sourceText && sourceFirstOctet > 0 && sourceFirstOctet != 127
        && sourceFirstOctet < 224 && !source.isLinkLocal();
}

void PrivateCloudDirectConnection::acceptReply(const QJsonObject &reply)
{
    const auto now = _now();
    if (reply.value(QStringLiteral("renewed")).toBool() != _ready || now >= _requestAt + leaseDurationMs || (_ready && now >= _expiresAt)) {
        fail(tr("The privateCloud direct connection lease expired or changed unexpectedly."));
        return;
    }
    const auto becomingReady = !_ready;
    const auto generation = _generation;
    if (_ready) {
        for (const auto &key : {QStringLiteral("lease"),
                                QStringLiteral("server"),
                                QStringLiteral("port"),
                                QStringLiteral("interfaceIndex"),
                                QStringLiteral("source"),
                                QStringLiteral("permitInstalled")}) {
            if (reply.value(key) != _lease.value(key)) {
                fail(tr("The privateCloud direct connection lease changed. Reconnecting."));
                return;
            }
        }
    } else {
        if (!_relay.start(QHostAddress(reply.value(QStringLiteral("server")).toString()),
                          static_cast<quint32>(reply.value(QStringLiteral("interfaceIndex")).toDouble()),
                          QHostAddress(reply.value(QStringLiteral("source")).toString()))) {
            fail(tr("Could not start the privateCloud direct connection relay."));
            return;
        }
        const auto proxy = _relay.proxy();
        if (!PrivateCloudTransport::setEndpoint(proxy.port(), proxy.user(), proxy.password())) {
            fail(tr("Could not configure the privateCloud direct connection relay."));
            return;
        }
        // Publishing the endpoint emits configurationChanged synchronously.
        // A subscriber may stop/restart us while closing its old requests.
        if (!_active || generation != _generation) {
            return;
        }
        _ready = true;
    }
    _lease = reply;
    if (becomingReady) {
        qCInfo(lcPrivateCloudDirect) << "Direct connection ready; physical interface" << reply.value(QStringLiteral("interfaceIndex")).toInt() << "server"
                                     << reply.value(QStringLiteral("server")).toString() << "exception installed"
                                     << reply.value(QStringLiteral("permitInstalled")).toBool();
    }
    _expiresAt = _requestAt + leaseDurationMs;
    _expiryTimer.start(static_cast<int>(qMax<qint64>(1, _expiresAt - _now())));
    _renewalTimer.start(renewalIntervalMs);
    _responseTimer.stop();
    _awaitingReply = false;
    _retryDelayMs = initialRetryMs;
    if (!_lastError.isEmpty()) {
        _lastError.clear();
        Q_EMIT errorChanged(_lastError);
    }
    // Publish readiness only after the complete lease state is installed.
    // Reentrant stop() must leave no timers or stale readiness behind.
    if (becomingReady && _active && generation == _generation && _ready) {
        Q_EMIT readyChanged(true);
    }
}

void PrivateCloudDirectConnection::renewLease()
{
    if (_active && _ready) {
        sendRequest(true);
    }
}

void PrivateCloudDirectConnection::expireLease()
{
    if (!_ready) {
        return;
    }
    if (_now() >= _expiresAt) {
        fail(tr("The privateCloud direct connection lease expired."));
    } else {
        _expiryTimer.start(static_cast<int>(_expiresAt - _now()));
    }
}

void PrivateCloudDirectConnection::closeSession()
{
    ++_generation;
    _responseTimer.stop();
    _renewalTimer.stop();
    _expiryTimer.stop();
    _awaitingReply = false;
    _reply.clear();
    _lease = {};
    if (_pipe) {
        _pipe->disconnect(this);
        _pipe->abort();
        _pipe->deleteLater();
        _pipe.clear();
    }
    PrivateCloudTransport::clearEndpoint();
    _relay.stop();
    if (_ready) {
        _ready = false;
        Q_EMIT readyChanged(false);
    }
}

void PrivateCloudDirectConnection::fail(const QString &error)
{
    if (_lastError != error) {
        qCWarning(lcPrivateCloudDirect) << error;
    }
    closeSession();
    _lastError = error;
    Q_EMIT errorChanged(_lastError);
    if (_active) {
        _retryTimer.start(_retryDelayMs);
        _retryDelayMs = qMin(_retryDelayMs * 2, maximumRetryMs);
    }
}
}
