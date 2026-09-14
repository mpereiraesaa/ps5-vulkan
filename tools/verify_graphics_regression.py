"""Shared GPU/compute/retirement checks for owned format diagnostics.

Case-specific pixel oracles and TCP/artifact validation remain in the caller.
This validates execution evidence, not conformance or deployment identity.
"""


def validate_regression(records, count):
    def require(value, message):
        if not value:
            raise ValueError(message)

    def matching(tag):
        return [(i, fields) for i, (name, fields) in enumerate(records) if name == tag]

    submits = matching("PS5VK_GRAPHICS_SUBMIT")
    completes = matching("PS5VK_GRAPHICS_COMPLETED")
    presents = matching("PS5VK_VIDEO_PRESENTED")
    retires = matching("PS5VK_GRAPHICS_REUSE_END")
    computes = matching("PS5VK_COMPUTE_RESULT")
    ends = matching("PS5VK_COMPUTE_END")
    require(count > 0 and all(len(g) == count for g in
            (submits, completes, presents, retires)), "GPU execution counts")
    require(len(computes) == count*12 and len(ends) == count*2 and
            all(f.get("outputs") == f.get("guards") == "0" and
                f.get("checked") == "3072" for _, f in computes) and
            all(f.get("rounds") == "6" and f.get("dispatches") == "12"
                for _, f in ends), "compute controls")
    previous = -1
    previous_serial = 0
    for case in range(count):
        s, c, p, r = (g[case] for g in (submits, completes, presents, retires))
        require(previous < ends[case*2][0] < s[0] < c[0] < p[0] < r[0] <
                ends[case*2+1][0], "GPU execution ordering")
        serial = int(s[1].get("serial", "0"))
        require(serial > previous_serial and s[1].get("serial") == c[1].get("serial")
                and s[1].get("rc") == "0" and p[1].get("fence") == "0"
                and p[1].get("matching_event") == "1" and
                r[1].get("displayed") == "0", "GPU completion and retirement")
        for phase in range(2):
            chunk = computes[(2*case+phase)*6:(2*case+phase+1)*6]
            lower = previous if phase == 0 else r[0]
            upper = ends[2*case+phase][0]
            require([f.get("round") for _, f in chunk] == list(map(str, range(6)))
                    and all(lower < i < upper for i, _ in chunk),
                    "compute phase ordering")
        previous = ends[2*case+1][0]
        previous_serial = serial
    platform = matching("PS5VK_PLATFORM_CLOSE")
    cleanup = matching("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    require(len(platform) == len(cleanup) == 1 and
            previous < platform[0][0] < cleanup[0][0] and
            platform[0][1].get("rc") == platform[0][1].get("allocations_bytes") == "0",
            "unique ordered resource cleanup")
