#!/usr/bin/env python3
"""Print the modem/IPA SoC addresses of this phone as boot-args, from its device tree.

usage: tools/ace5/soc-args.py DEVICE-TREE.dtb [--check]

Adapted from hackios-xnu-kernel tools/soc-args.py (MIT License, oosexgang) for
the Ace 5 / SM8650 ("pineapple"). The upstream matcher assumes POCO X5 Pro node
names; on this SoC several differ, and the ones below are matched by compatible
or by the reference the driver actually follows:

  aoss        qcom,aoss-qmp              (the SoC prefix in the compatible varies)
  cmd_db      qcom,cmd-db                (the node is aop_cmd_db_region@...)
  smem        qcom,smem                  (on the reservation itself)
  mpss        remoteproc-mss's memory-region phandle, not the first node named
              mpss: mpss_dsm_region@... comes before mpss_region@...
  rsc         the rpmh-rsc node whose reg-names has drv-2
  ipa_gsi     the ipa_gsi_region reservation

A value the device tree does not carry is printed as a comment, never guessed.

--check prints a one-line verdict per argument instead of the boot-arg lines.
"""
import struct
import sys


def nodes_of(blob):
    if blob[:4] != b'\xd0\x0d\xfe\xed':
        sys.exit('soc-args.py: not a flattened device tree')
    off_struct, off_strings = struct.unpack('>II', blob[8:16])
    size_strings, = struct.unpack('>I', blob[32:36])
    strings = blob[off_strings:off_strings + size_strings]
    out, path, at = {}, [], off_struct
    while True:
        token, = struct.unpack('>I', blob[at:at + 4])
        at += 4
        if token == 1:                                  # FDT_BEGIN_NODE
            end = blob.index(b'\0', at)
            path.append(blob[at:end].decode())
            at = (end + 4) & ~3
            out['/'.join(path)] = {}
        elif token == 2:                                # FDT_END_NODE
            path.pop()
        elif token == 3:                                # FDT_PROP
            length, name_off = struct.unpack('>II', blob[at:at + 8])
            at += 8
            name = strings[name_off:strings.index(b'\0', name_off)].decode()
            out['/'.join(path)][name] = blob[at:at + length]
            at = (at + length + 3) & ~3
        elif token == 9:                                # FDT_END
            return out
        elif token != 4:                                # 4 = FDT_NOP
            sys.exit(f'soc-args.py: unexpected token {token}')


def u32(value, index=0):
    return struct.unpack_from('>I', value, index * 4)[0]


def compatibles(props):
    return props.get('compatible', b'').split(b'\0')


def find_compat(nodes, compatible):
    for path, props in nodes.items():
        if compatible.encode() in compatibles(props):
            return path, props
    return None, None


def find_leaf(nodes, leaf):
    """The node whose last path component begins with `leaf` (e.g. apps-smmu@...)."""
    for path, props in nodes.items():
        if path.rsplit('/', 1)[-1].split('@', 1)[0] == leaf:
            return path, props
    return None, None


def find_compat_containing(nodes, needle):
    for path, props in nodes.items():
        if any(needle.encode() in c for c in compatibles(props)):
            return path, props
    return None, None


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    if len(argv) != 1:
        sys.exit(__doc__)
    nodes = nodes_of(open(argv[0], 'rb').read())
    args, missing = [], []

    def add(name, value, source):
        args.append((name, value, source))

    def skip(name, why):
        missing.append((name, why))

    path, ipa = find_compat(nodes, 'qcom,ipa')
    if ipa and 'reg' in ipa and len(ipa['reg']) >= 16:
        add('soc_ipa_base', u32(ipa['reg'], 0) + 0x40000, path)
        add('soc_gsi_base', u32(ipa['reg'], 2), path)
    else:
        skip('soc_ipa_base', 'no qcom,ipa node with two address/size pairs')

    for compat, arg in (('qcom,ipcc', 'soc_ipcc'),
                        ('qcom,aoss-qmp', 'soc_aoss')):
        path, props = find_compat(nodes, compat)
        if props and 'reg' in props:
            add(arg, u32(props['reg'], 0), path)
        else:
            skip(arg, f'no {compat} node')

    for path, props in nodes.items():
        if b'qcom,rpmh-rsc' not in compatibles(props) or 'reg-names' not in props:
            continue
        names = props['reg-names'].split(b'\0')
        if b'drv-2' in names:
            add('soc_rsc', u32(props['reg'], names.index(b'drv-2') * 2), path)
            break
    else:
        skip('soc_rsc', 'no rpmh-rsc node with drv-2')

    path, smmu = find_leaf(nodes, 'apps-smmu')
    if smmu and 'reg' in smmu:
        add('soc_apps_smmu', u32(smmu['reg'], 0), path)
    else:
        skip('soc_apps_smmu', 'no apps-smmu node')

    by_phandle = {}
    for path, props in nodes.items():
        if 'phandle' in props:
            by_phandle[u32(props['phandle'])] = (path, props)

    def reserved_by_compat(compat, arg):
        path, props = find_compat(nodes, compat)
        if props and 'reg' in props:
            add(arg, u32(props['reg'], 1), path)         # reserved-memory: 2 address cells
        else:
            skip(arg, f'no reserved-memory node with compatible {compat}')

    reserved_by_compat('qcom,cmd-db', 'soc_cmd_db')
    reserved_by_compat('qcom,smem', 'soc_smem')

    path, props = find_leaf(nodes, 'ipa_gsi_region')
    if props and 'reg' in props:
        add('modem_ipa_fw_at', u32(props['reg'], 1), path)
    else:
        skip('modem_ipa_fw_at', 'no ipa_gsi_region reservation')

    # The modem's own memory: the reservation its remoteproc node points at.
    path, mss = find_compat_containing(nodes, 'modem-pas')
    if mss and 'memory-region' in mss:
        target = by_phandle.get(u32(mss['memory-region']))
        if target and 'reg' in target[1]:
            add('soc_mpss_mem', u32(target[1]['reg'], 1), target[0])
        else:
            skip('soc_mpss_mem', 'memory-region phandle does not resolve')
    else:
        skip('soc_mpss_mem', 'no modem-pas remoteproc node')

    path, imem = find_compat(nodes, 'qcom,msm-imem')
    if imem and 'reg' in imem:
        add('soc_imem', u32(imem['reg'], 0) & ~0x1fffff, path)
    else:
        skip('soc_imem', 'no qcom,msm-imem node')

    for compat, arg in (('qcom,ipa-smmu-ap-cb', 'soc_ipa_stream'),
                        ('qcom,ipa-smmu-uc-cb', 'soc_ipa_stream_uc')):
        path, props = find_compat(nodes, compat)
        if props and 'iommus' in props and len(props['iommus']) >= 8:
            add(arg, u32(props['iommus'], 1), path)
        else:
            skip(arg, f'no {compat} node with iommus')

    path, ipa_fws = find_leaf(nodes, 'qcom,ipa_fws')
    if ipa_fws and 'qcom,pas-id' in ipa_fws:
        add('soc_pas_ipa', u32(ipa_fws['qcom,pas-id']), path)
    else:
        skip('soc_pas_ipa', 'no qcom,pas-id on qcom,ipa_fws')

    # The modem's PAS id: POCO's tree carries it, this one does not. The kernel
    # keeps its built-in default; do not invent a value here.
    if mss and 'qcom,pas-id' in mss:
        add('soc_pas_mpss', u32(mss['qcom,pas-id']), path)
    else:
        skip('soc_pas_mpss', 'no qcom,pas-id on the modem-pas node (kernel default stays)')

    ipa_fw_name = None
    if ipa_fws and 'qcom,firmware-name' in ipa_fws:
        ipa_fw_name = ipa_fws['qcom,firmware-name'].split(b'\0')[0].decode()

    if check:
        for name, value, source in args:
            print(f'{name:20s} {value:#x}  <- {source}')
        for name, why in missing:
            print(f'{name:20s} {"missing":>10}  {why}')
        if ipa_fw_name:
            print(f'{"soc_ipa_fw":20s} {ipa_fw_name:>10}')
        return 0

    for name, value, source in args:
        print(f'{name}={value:#x}')
    if ipa_fw_name:
        print(f'soc_ipa_fw={ipa_fw_name}')
    for name, why in missing:
        print(f'# {name}: {why}')
    if not args:
        sys.exit('soc-args.py: found none of the expected nodes; is this the phone\'s own device tree?')
    return 0


if __name__ == '__main__':
    sys.exit(main())
