#pragma once
#include "policy_gate.h"
#include <fwpmu.h>
#include <functional>
#include <vector>

namespace policy_detail
{
struct InspectionFailure {
    const char *reason = nullptr;
    const char *stage = nullptr;
    DWORD nativeError = 0;
    bool record(const char *atStage, const char *why, DWORD error = 0)
    {
        if (!reason) {
            stage = atStage;
            reason = why;
            nativeError = error;
        }
        return false;
    }
    PolicyProof proof() const
    {
        PolicyProof result;
        result.reason = reason ? reason : "policy-snapshot-or-identity-unverified";
        result.stage = stage ? stage : "policy-snapshot";
        result.nativeError = nativeError;
        return result;
    }
};

// Tailscale v1.102.3 uses SCM service -> /subproc -> /firewall. No further
// descendants are admitted. Native capture pins every process while checking.
inline constexpr size_t maximumAncestryProcesses = 3;
struct ProcessIdentity {
    DWORD pid = 0;
    DWORD parent = 0;
    DWORD recheckedParent = 0;
    UINT64 created = 0;
    UINT64 recheckedCreated = 0;
    bool alive = false;
    bool imageMatches = false;
    bool systemUser = false;
};
bool assessAncestry(const std::vector<ProcessIdentity> &chain, DWORD processPid, DWORD servicePid, InspectionFailure &failure);
bool sameAncestry(const std::vector<ProcessIdentity> &before, const std::vector<ProcessIdentity> &after);
enum class Action {
    Permit,
    Block,
    Callout,
    Other,
};
enum class Shape {
    Other,
    CatchAll,
    Service,
    Tunnel,
    Loopback,
    Dns,
};
struct Rule {
    unsigned layer = 0;
    Action action = Action::Other;
    Shape shape = Shape::Other;
    bool tailscale = false;
    bool own = false;
    bool disjoint = false;
    bool flagsSupported = true;
    UINT16 sublayerWeight = 0;
    UINT64 filterWeight = 0;
    UINT64 tunnel = 0;
};
struct Facts {
    bool complete = false;
    bool routeUp = false;
    bool routeHardware = false;
    UINT64 routeLuid = 0;
    bool hasTailscale = false;
    bool serviceTrusted = false;
    bool sessionTrusted = false;
    bool metadataTrusted = false;
    GUID provider{};
    GUID sublayer{};
    std::vector<Rule> rules;
};
PolicyProof assess(const Facts &facts);

// WFP session enumeration cannot run inside a read transaction. Keep this
// ordering in one testable workflow; native callbacks contain the OS calls.
struct SnapshotOperations {
    std::function<void()> captureIdentity;
    std::function<bool()> beginRead;
    std::function<bool()> captureRules;
    std::function<bool()> endRead;
    std::function<bool()> identityUnchanged;
};
bool collectSnapshot(bool &hasTailscale, const SnapshotOperations &operations);
Shape classifyFilterShape(const FWPM_FILTER0 &filter, const FWP_BYTE_BLOB *serviceApp, UINT64 &luid);
bool filterDefinitelyDisjoint(const FWPM_FILTER0 &filter, IN_ADDR server);
}
