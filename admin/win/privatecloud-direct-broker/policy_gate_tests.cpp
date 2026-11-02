// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <initguid.h>
#include "policy_gate_detail.h"
#include <cstdio>
#include <cstring>
#include <array>
#include <stdexcept>

namespace
{
int failures = 0;
int checks = 0;
void check(bool passed, const char *name)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
}

policy_detail::Facts recognizedFacts()
{
    using namespace policy_detail;
    Facts facts;
    facts.complete = facts.routeUp = facts.hasTailscale = true;
    facts.serviceTrusted = facts.sessionTrusted = facts.metadataTrusted = true;
    facts.routeLuid = 123;
    facts.provider.Data1 = 1;
    facts.sublayer.Data1 = 2;
    for (unsigned layer = 0; layer != 2; ++layer) {
        for (const auto shape : {Shape::CatchAll, Shape::Service, Shape::Tunnel, Shape::Loopback, Shape::Dns}) {
            Rule rule;
            rule.layer = layer;
            rule.action = shape == Shape::CatchAll ? Action::Block : Action::Permit;
            rule.shape = shape;
            rule.tailscale = true;
            rule.sublayerWeight = 1;
            rule.filterWeight = shape == Shape::CatchAll ? 0 : 15;
            rule.tunnel = facts.routeLuid;
            facts.rules.push_back(rule);
        }
    }
    return facts;
}

template<class Change>
void rejected(const char *name, Change change)
{
    auto facts = recognizedFacts();
    change(facts);
    check(policy_detail::assess(facts).state == PolicyState::Untrusted, name);
}

void assessTests()
{
    using namespace policy_detail;
    const auto good = recognizedFacts();
    const auto proof = assess(good);
    check(proof.state == PolicyState::RecognizedTailscale && proof.tunnel.Value == good.routeLuid && IsEqualGUID(proof.provider, good.provider)
              && IsEqualGUID(proof.sublayer, good.sublayer),
          "recognized proof retains identity");
    rejected("incomplete snapshot", [](auto &f) {
        f.complete = false;
    });
    rejected("down route", [](auto &f) {
        f.routeUp = false;
    });
    rejected("missing route", [](auto &f) {
        f.routeLuid = 0;
    });
    rejected("untrusted service", [](auto &f) {
        f.serviceTrusted = false;
    });
    rejected("untrusted session", [](auto &f) {
        f.sessionTrusted = false;
    });
    rejected("untrusted metadata", [](auto &f) {
        f.metadataTrusted = false;
    });
    rejected("policy without exit route", [](auto &f) {
        f.routeHardware = true;
    });
    rejected("unknown virtual route", [](auto &f) {
        f.hasTailscale = false;
    });
    rejected("missing required rule", [](auto &f) {
        f.rules.pop_back();
    });
    rejected("duplicate required rule", [](auto &f) {
        f.rules.push_back(f.rules.front());
    });
    rejected("wrong route LUID", [](auto &f) {
        f.rules[2].tunnel = 999;
    });
    rejected("unsupported layer", [](auto &f) {
        f.rules[0].layer = 2;
    });
    rejected("unsupported flags", [](auto &f) {
        f.rules[0].flagsSupported = false;
    });
    rejected("unexpected Tailscale sublayer priority", [](auto &f) {
        f.rules[0].sublayerWeight = 256;
    });
    rejected("changed block weight", [](auto &f) {
        f.rules[0].filterWeight = 15;
    });
    rejected("changed catch-all shape", [](auto &f) {
        f.rules[0].shape = Shape::Other;
    });
    rejected("changed permit weight", [](auto &f) {
        f.rules[1].filterWeight = 14;
    });
    rejected("changed permit shape", [](auto &f) {
        f.rules[1].shape = Shape::Other;
    });
    rejected("Tailscale callout", [](auto &f) {
        f.rules[0].action = Action::Callout;
    });
    auto physical = good;
    physical.hasTailscale = false;
    physical.routeHardware = true;
    check(assess(physical).state == PolicyState::NoBypassNeeded, "no exit-node policy needs no exception");
    for (const auto action : {Action::Block, Action::Callout, Action::Other}) {
        for (const auto weight : std::array<UINT16, 6>{0, 1, 255, 256, 257, 511}) {
            auto facts = good;
            Rule competing;
            competing.action = action;
            competing.sublayerWeight = weight;
            facts.rules.push_back(competing);
            check(assess(facts).state == PolicyState::RecognizedTailscale, "approved exact privateCloud exception accepts potentially matching lower policy");
            facts.rules.back().disjoint = true;
            check(assess(facts).state == PolicyState::RecognizedTailscale, "proven disjoint lower policy is harmless");
        }
    }
    for (const auto weight : std::array<UINT16, 4>{512, 513, 768, 65535}) {
        auto facts = good;
        Rule block;
        block.action = Action::Block;
        block.sublayerWeight = weight;
        facts.rules.push_back(block);
        check(assess(facts).state == PolicyState::RecognizedTailscale, "higher policy arbitration belongs to WFP, not Tailscale recognition");
    }
    auto unrelatedLayout = good;
    Rule unrelated;
    unrelated.action = Action::Callout;
    unrelated.layer = 2;
    unrelated.flagsSupported = false;
    unrelatedLayout.rules.push_back(unrelated);
    check(assess(unrelatedLayout).state == PolicyState::RecognizedTailscale, "unrelated policy layout is outside approved Tailscale recognition contract");
    unrelatedLayout.serviceTrusted = false;
    check(assess(unrelatedLayout).state == PolicyState::Untrusted, "approved exception still rejects untrusted Tailscale identity");
    unrelatedLayout = good;
    unrelatedLayout.rules.push_back(unrelated);
    unrelatedLayout.rules.front().flagsSupported = false;
    check(assess(unrelatedLayout).state == PolicyState::Untrusted, "approved exception still rejects unsupported Tailscale layout");
    unrelatedLayout = good;
    unrelatedLayout.rules.pop_back();
    unrelatedLayout.rules.push_back(unrelated);
    check(assess(unrelatedLayout).state == PolicyState::Untrusted, "approved exception still requires complete Tailscale rule shape");
    auto own = good;
    Rule permit;
    permit.own = true;
    permit.action = Action::Permit;
    own.rules.push_back(permit);
    check(assess(own).state == PolicyState::RecognizedTailscale, "own temporary permit accepted");
    own.rules.back().action = Action::Block;
    check(assess(own).state == PolicyState::Untrusted, "unexpected own blocking filter rejected");
    check(!privateCloudPermitWeightMatches(1) && !privateCloudPermitWeightMatches(255) && privateCloudPermitWeightMatches(256)
              && privateCloudPermitWeightMatches(257) && privateCloudPermitWeightMatches(511) && !privateCloudPermitWeightMatches(512),
          "normalized permit priority class boundary");
}

void conditionTests()
{
    using namespace policy_detail;
    IN_ADDR server{};
    server.S_un.S_addr = htonl(0x08080808);
    FWPM_FILTER_CONDITION0 conditions[3]{};
    FWPM_FILTER0 filter{};
    UINT64 luid = 0;
    check(classifyFilterShape(filter, nullptr, luid) == Shape::CatchAll, "zero-condition catch-all");
    filter.numFilterConditions = 1;
    check(classifyFilterShape(filter, nullptr, luid) == Shape::Other && !filterDefinitelyDisjoint(filter, server), "null conditions fail closed");
    filter.filterCondition = conditions;
    conditions[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    conditions[0].matchType = FWP_MATCH_EQUAL;
    conditions[0].conditionValue.type = FWP_UINT8;
    conditions[0].conditionValue.uint8 = IPPROTO_UDP;
    check(filterDefinitelyDisjoint(filter, server), "UDP is disjoint from TCP");
    conditions[1] = conditions[0];
    conditions[1].conditionValue.uint8 = IPPROTO_TCP;
    filter.numFilterConditions = 2;
    check(!filterDefinitelyDisjoint(filter, server), "repeated protocol is OR, not AND");
    conditions[2].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
    conditions[2].matchType = FWP_MATCH_EQUAL;
    conditions[2].conditionValue.type = FWP_UINT16;
    conditions[2].conditionValue.uint16 = 53;
    filter.numFilterConditions = 3;
    check(filterDefinitelyDisjoint(filter, server) && classifyFilterShape(filter, nullptr, luid) == Shape::Dns, "DNS OR protocols remain disjoint on port");
    conditions[2].conditionValue.uint16 = 443;
    check(!filterDefinitelyDisjoint(filter, server) && classifyFilterShape(filter, nullptr, luid) == Shape::Other, "HTTPS port cannot be classified as DNS");
    filter.numFilterConditions = 1;
    conditions[0] = conditions[2];
    conditions[0].conditionValue.uint16 = 80;
    check(filterDefinitelyDisjoint(filter, server), "unrelated port disjoint");
    conditions[1] = conditions[2];
    filter.numFilterConditions = 2;
    check(!filterDefinitelyDisjoint(filter, server), "repeated ports include HTTPS");
    filter.numFilterConditions = 1;
    conditions[0] = {};
    conditions[0].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    conditions[0].matchType = FWP_MATCH_EQUAL;
    conditions[0].conditionValue.type = FWP_UINT32;
    conditions[0].conditionValue.uint32 = 0x01010101;
    check(filterDefinitelyDisjoint(filter, server), "different remote address disjoint");
    conditions[1] = conditions[0];
    conditions[1].conditionValue.uint32 = 0x08080808;
    filter.numFilterConditions = 2;
    check(!filterDefinitelyDisjoint(filter, server), "repeated address includes server");
    filter.numFilterConditions = 1;
    FWP_V4_ADDR_AND_MASK mask{0x08080000, 0xffff0000};
    conditions[0].conditionValue.type = FWP_V4_ADDR_MASK;
    conditions[0].conditionValue.v4AddrMask = &mask;
    check(!filterDefinitelyDisjoint(filter, server), "subnet containing server overlaps");
    mask.addr = 0x08090000;
    check(filterDefinitelyDisjoint(filter, server), "different subnet disjoint");
    conditions[0].matchType = FWP_MATCH_NOT_EQUAL;
    check(!filterDefinitelyDisjoint(filter, server), "unsupported inequality cannot prove disjoint");
    conditions[0] = {};
    conditions[0].fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
    conditions[0].matchType = FWP_MATCH_EQUAL;
    conditions[0].conditionValue.type = FWP_UINT64;
    UINT64 tunnel = 123;
    conditions[0].conditionValue.uint64 = &tunnel;
    check(classifyFilterShape(filter, nullptr, luid) == Shape::Tunnel && luid == tunnel, "exact tunnel interface shape");
    conditions[0].conditionValue.uint64 = nullptr;
    check(classifyFilterShape(filter, nullptr, luid) == Shape::Other, "missing interface value refused");
    conditions[0] = {};
    conditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
    conditions[0].matchType = FWP_MATCH_EQUAL;
    conditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
    BYTE bytes[]{1, 2, 3};
    FWP_BYTE_BLOB app{3, bytes};
    conditions[0].conditionValue.byteBlob = &app;
    check(classifyFilterShape(filter, &app, luid) == Shape::Service, "exact service app identity");
    BYTE otherBytes[]{1, 2, 4};
    FWP_BYTE_BLOB other{3, otherBytes};
    check(classifyFilterShape(filter, &other, luid) == Shape::Other, "other executable identity refused");
}

void snapshotOrderTests()
{
    for (const auto scenario : {0, 1, 2, 3, 4, 5}) {
        bool transaction = false, hasTailscale = false;
        bool identityOutsideTransaction = true, rulesInsideTransaction = true;
        int identityReads = 0, endReads = 0, ruleReads = 0;
        const policy_detail::SnapshotOperations operations{[&] {
                                                               identityOutsideTransaction &= !transaction;
                                                               ++identityReads;
                                                           },
                                                           [&] {
                                                               transaction = scenario != 1;
                                                               return transaction;
                                                           },
                                                           [&] {
                                                               rulesInsideTransaction &= transaction;
                                                               ++ruleReads;
                                                               hasTailscale = scenario != 4;
                                                               if (scenario == 5) {
                                                                   throw std::runtime_error("fixture failure");
                                                               }
                                                               return scenario != 2;
                                                           },
                                                           [&] {
                                                               ++endReads;
                                                               transaction = false;
                                                               return true;
                                                           },
                                                           [&] {
                                                               identityOutsideTransaction &= !transaction;
                                                               ++identityReads;
                                                               return scenario != 3;
                                                           }};
        bool result = false;
        bool threw = false;
        try {
            result = policy_detail::collectSnapshot(hasTailscale, operations);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        check(identityOutsideTransaction && rulesInsideTransaction && !transaction, "identity reads stay outside transaction and rules inside it");
        check(result == (scenario == 0 || scenario == 4), "snapshot failure or identity change refuses recognition");
        check(endReads == (scenario == 1 ? 0 : 1) && ruleReads == (scenario == 1 ? 0 : 1), "every started transaction is closed even on failure");
        check(identityReads == (scenario == 0 || scenario == 3 ? 2 : 1) && threw == (scenario == 5), "only complete Tailscale snapshot rechecks identity");
    }
}

void diagnosticTests()
{
    policy_detail::InspectionFailure failure;
    check(!failure.record("tailscale-service-process", "process-token-open-failed", ERROR_ACCESS_DENIED), "recording a native failure refuses inspection");
    failure.record("read-transaction", "policy-read-transaction-close-failed", ERROR_INVALID_HANDLE);
    const auto proof = failure.proof();
    check(proof.state == PolicyState::Untrusted && std::strcmp(proof.stage, "tailscale-service-process") == 0
              && std::strcmp(proof.reason, "process-token-open-failed") == 0 && proof.nativeError == ERROR_ACCESS_DENIED,
          "first native cause survives later cleanup failure");
    policy_detail::InspectionFailure metadata;
    metadata.record("tailscale-sublayer-metadata", "sublayer-flags-unexpected");
    const auto metadataProof = metadata.proof();
    check(metadataProof.state == PolicyState::Untrusted && metadataProof.nativeError == 0
              && std::strcmp(metadataProof.stage, "tailscale-sublayer-metadata") == 0,
          "metadata mismatch has its own stage without invented Windows error");
    const auto fallback = policy_detail::InspectionFailure{}.proof();
    check(fallback.state == PolicyState::Untrusted && fallback.reason && fallback.stage && !fallback.nativeError,
          "missing diagnostic detail stays untrusted with bounded static codes");
}

std::vector<policy_detail::ProcessIdentity> expectedAncestry()
{
    return {{300, 200, 200, 3000, 3000, true, true, true}, {200, 100, 100, 2000, 2000, true, true, true}, {100, 50, 50, 1000, 1000, true, true, true}};
}

void ancestryTests()
{
    using namespace policy_detail;
    const auto good = expectedAncestry();
    InspectionFailure failure;
    check(good.front().parent != good.back().pid, "regression fixture fails the former direct-parent-only check");
    check(assessAncestry(good, 300, 100, failure), "authenticated service child and firewall grandchild are accepted");
    for (const auto first : {1, 2}) {
        const std::vector<ProcessIdentity> shorter(good.begin() + first, good.end());
        InspectionFailure shortFailure;
        check(assessAncestry(shorter, shorter.front().pid, 100, shortFailure), "authenticated direct child or service process remains valid");
    }
    const auto refuse = [&](const char *name, const auto &change) {
        auto chain = good;
        change(chain);
        InspectionFailure rejected;
        check(!assessAncestry(chain, 300, 100, rejected) && rejected.reason, name);
    };
    refuse("empty ancestry refused", [](auto &chain) {
        chain.clear();
    });
    refuse("different starting process refused", [](auto &chain) {
        chain.front().pid = 301;
    });
    refuse("ancestry outside SCM service refused", [](auto &chain) {
        chain.back().pid = 101;
    });
    refuse("missing intermediate node refused", [](auto &chain) {
        chain.erase(chain.begin() + 1);
    });
    refuse("parent PID reuse after child creation refused", [](auto &chain) {
        chain[1].created = chain[1].recheckedCreated = 4000;
    });
    refuse("creation identity changed during capture refused", [](auto &chain) {
        ++chain[1].recheckedCreated;
    });
    refuse("parent snapshot changed during capture refused", [](auto &chain) {
        ++chain[1].recheckedParent;
    });
    refuse("unavailable creation identity refused", [](auto &chain) {
        chain[1].created = chain[1].recheckedCreated = 0;
    });
    refuse("ancestry cycle refused", [](auto &chain) {
        chain[1].pid = chain[0].pid;
    });
    refuse("extra descendant beyond documented two edges refused", [](auto &chain) {
        chain.insert(chain.begin() + 1, {250, 200, 200, 2500, 2500, true, true, true});
        chain.front().parent = chain.front().recheckedParent = 250;
    });
    for (size_t index = 0; index < good.size(); ++index) {
        refuse("any exited ancestry process refused", [index](auto &chain) {
            chain[index].alive = false;
        });
        refuse("any different image in ancestry refused", [index](auto &chain) {
            chain[index].imageMatches = false;
        });
        refuse("any non-SYSTEM ancestry process refused", [index](auto &chain) {
            chain[index].systemUser = false;
        });
    }
    check(sameAncestry(good, good), "unchanged pinned ancestry matches across policy snapshot");
    auto reused = good;
    ++reused[1].created;
    check(!sameAncestry(good, reused), "same numeric PID with another lifetime cannot match snapshot");
    auto changedParent = good;
    ++changedParent[1].parent;
    check(!sameAncestry(good, changedParent), "changed parent cannot match snapshot");
    auto changedSize = good;
    changedSize.pop_back();
    check(!sameAncestry(good, changedSize), "changed chain length cannot match snapshot");
}
}

int main()
{
    assessTests();
    conditionTests();
    snapshotOrderTests();
    diagnosticTests();
    ancestryTests();
    std::printf("Policy gate: %d checks, %d failures. No OS policy, service, DNS, socket or registry operations.\n", checks, failures);
    return failures ? 1 : 0;
}
