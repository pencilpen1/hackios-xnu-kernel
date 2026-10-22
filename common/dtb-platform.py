#!/usr/bin/env python3
"""Extract loader platform fields from a DTB using dtc's fdtget."""
import subprocess
import sys


def inspect(dtb):
    def get(node, prop, kind="x", default=None):
        p = subprocess.run(["fdtget", "-t", kind, dtb, node, prop],
                           capture_output=True, text=True)
        if p.returncode:
            if default is not None:
                return default
            raise ValueError(f"{node}: missing {prop}")
        return p.stdout.strip()

    def cells(node, prop):
        return [int(v, 16) for v in get(node, prop).split()]

    def nodes(node="/"):
        yield node
        children = subprocess.check_output(["fdtget", "-l", dtb, node], text=True)
        for child in children.splitlines():
            yield from nodes(node.rstrip("/") + "/" + child)

    gic = None
    cpus = []
    for node in nodes():
        compat = get(node, "compatible", "s", "").split()
        if "arm,gic-v3" in compat:
            parent = node.rsplit("/", 1)[0] or "/"
            ac = int(get(parent, "#address-cells", default="2"), 16)
            sc = int(get(parent, "#size-cells", default="1"), 16)
            if ac not in (1, 2) or sc not in (1, 2):
                raise ValueError("unsupported GIC reg cell width")
            # Translating a bus window requires more than concatenating reg cells.
            if parent != "/" and get(parent, "ranges", default="absent"):
                raise ValueError("GIC behind a translated bus: resolve ranges manually")
            reg = cells(node, "reg")
            if len(reg) < 2 * (ac + sc):
                raise ValueError("GIC distributor/redistributor reg is incomplete")
            def address(offset):
                value = 0
                for cell in reg[offset:offset + ac]:
                    value = (value << 32) | cell
                return value
            gic = (address(0), address(ac + sc))
        if node.startswith("/cpus/") and get(node, "device_type", "s", "") == "cpu":
            if get(node, "status", "s", "okay") == "disabled":
                continue
            reg = cells(node, "reg")
            mpidr = 0
            for cell in reg:
                mpidr = (mpidr << 32) | cell
            cpus.append((mpidr, int(get(node, "capacity-dmips-mhz", default="0"), 16)))
    if not gic or not cpus or len(cpus) > 8:
        raise ValueError("need GICv3 and 1..8 enabled CPU nodes")
    ids = sorted(r for r, _ in cpus)
    shift = next((s for s in (0, 8) if ids == [i << s for i in range(len(ids))]), None)
    if shift is None:
        raise ValueError("MPIDRs need an explicit topology; loader core_shift cannot represent them")
    caps = [c for _, c in cpus]
    small = sum(1 << (r >> shift) for r, c in cpus
                if min(caps) > 0 and min(caps) < max(caps) and c == min(caps))
    method = get("/psci", "method", "s", "smc")
    if method not in ("smc", "hvc"):
        raise ValueError("unknown PSCI method")
    return *gic, len(cpus), shift, small, int(method == "hvc")


if __name__ == "__main__":
    try:
        dist, redist, cores, shift, small, hvc = inspect(sys.argv[1])
        print(hex(dist), hex(redist), cores, shift, hex(small), hvc)
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
