#!/usr/bin/env python3
"""Generate the PCIe0 register header for giulia from the phone's own device tree.

The vendor device tree carries the pieces a hand-written PCIe bring-up needs and
that no Linux table has for this board: the register window bases, the PERST /
CLKREQ / WAKE pins, the PHY init sequence and the PCIe state-machine sequence.
This turns them into `xnu/files/bsd/dev/giulia_pcie_regs.h` instead of copying
another phone's constants.

    python3 tools/ace5/pcie-regs.py dtb/giulia.dtb

The GCC offsets at the end of the header are not from the device tree: they are
the branch/GDSC/reset offsets from Linux's drivers/clk/qcom/gcc-sm8650.c, kept in
this GPL-2.0-only file the same way giulia_gpu_regs.h keeps KGSL's tables.
"""
import argparse
import struct
import sys

PCIE = '/soc/qcom,pcie@1c00000'
CNSS = '/soc/qcom,cnss-kiwi@b0000000'
TLMM = '/soc/pinctrl@f000000'


def parse(data):
    magic, total, off_struct, off_strings, _, version, last, bootid, size_strings, size_struct = \
        struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed:
        raise SystemExit('not a flattened device tree')
    strings = data[off_strings:off_strings + size_strings]
    p, stack, nodes = off_struct, [], {}
    end = off_struct + size_struct
    while p < end:
        token = struct.unpack_from('>I', data, p)[0]
        p += 4
        if token == 1:
            nul = data.index(0, p)
            stack.append(data[p:nul].decode('utf-8', 'replace'))
            nodes['/'.join(stack) or '/'] = {}
            p = (nul + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, offset = struct.unpack_from('>II', data, p)
            p += 8
            nul = strings.index(0, offset)
            name = strings[offset:nul].decode('utf-8', 'replace')
            nodes['/'.join(stack) or '/'][name] = data[p:p + size]
            p = (p + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            break
        else:
            raise SystemExit('bad FDT token %d' % token)
    return nodes


def words(value):
    if not value or len(value) % 4:
        return []
    return list(struct.unpack('>%dI' % (len(value) // 4), value))


def names(value):
    return [part for part in value.rstrip(b'\0').decode('utf-8', 'replace').split('\0') if part]


def pin(prop, tlmm_phandle, label):
    w = words(prop)
    if len(w) != 3:
        raise SystemExit('%s: expected <phandle pin flags>, got %r' % (label, w))
    if w[0] != tlmm_phandle:
        raise SystemExit('%s: phandle %#x is not the TLMM (%#x)' % (label, w[0], tlmm_phandle))
    return w[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dtb')
    ap.add_argument('--out', default='xnu/files/bsd/dev/giulia_pcie_regs.h')
    args = ap.parse_args()

    with open(args.dtb, 'rb') as handle:
        nodes = parse(handle.read())

    pcie = nodes.get(PCIE)
    cnss = nodes.get(CNSS)
    tlmm = nodes.get(TLMM)
    if not pcie or not cnss or not tlmm:
        raise SystemExit('missing pcie/cnss/tlmm node in %s' % args.dtb)

    tlmm_phandle = words(tlmm.get('phandle'))[0]
    regs = words(pcie['reg'])
    reg_names = names(pcie['reg-names'])
    if len(regs) != 2 * len(reg_names):
        raise SystemExit('reg/reg-names length mismatch: %d vs %d' % (len(regs), len(reg_names)))
    base = {name: regs[2 * i] for i, name in enumerate(reg_names)}
    size = {name: regs[2 * i + 1] for i, name in enumerate(reg_names)}

    perst = pin(pcie['perst-gpio'], tlmm_phandle, 'perst-gpio')
    clkreq = words(pcie['qcom,pcie-clkreq-gpio'])[0]
    wake = pin(pcie['wake-gpio'], tlmm_phandle, 'wake-gpio')
    wlan_en = pin(cnss['wlan-en-gpio'], tlmm_phandle, 'wlan-en-gpio')
    bt_en = pin(cnss['qcom,bt-en-gpio'], tlmm_phandle, 'qcom,bt-en-gpio')
    sw_ctrl = pin(cnss['qcom,sw-ctrl-gpio'], tlmm_phandle, 'qcom,sw-ctrl-gpio')
    wlan_sw_ctrl = pin(cnss['qcom,wlan-sw-ctrl-gpio'], tlmm_phandle, 'qcom,wlan-sw-ctrl-gpio')

    phy_seq = words(pcie['qcom,phy-sequence'])
    if len(phy_seq) % 3:
        raise SystemExit('phy-sequence is not offset/value/delay triplets')
    sm_seq = words(pcie['qcom,pcie-sm-seq'])
    sm_branch = words(pcie['qcom,pcie-sm-branch-seq'])

    out = []
    w = out.append
    w('/* SPDX-License-Identifier: GPL-2.0-only */')
    w('/*')
    w(' * PCIe0 / WCN7850 constants for giulia (OnePlus Ace 5, SM8650).')
    w(' *')
    w(' * Generated from dtb/giulia.dtb by tools/ace5/pcie-regs.py. The register')
    w(' * windows, pins and the PHY/SM sequences are this board\'s vendor data; the')
    w(' * GCC offsets at the end are from Linux drivers/clk/qcom/gcc-sm8650.c')
    w(' * (GPL-2.0), kept here rather than in the APSL driver.')
    w(' */')
    w('#ifndef GIULIA_PCIE_REGS_H')
    w('#define GIULIA_PCIE_REGS_H')
    w('')
    for name in reg_names:
        macro = 'GIULIA_PCIE_' + name.upper().replace('-', '_')
        w('#define %-28s 0x%08xu' % (macro, base[name]))
        w('#define %-28s 0x%08xu' % (macro + '_SIZE', size[name]))
    w('')
    w('/* Root-complex window: io then mem, from the node\'s ranges. */')
    ranges = words(pcie['ranges'])
    for i in range(0, len(ranges), 6):
        kind = 'IO' if ranges[i] == 0x1000000 else 'MEM'
        w('#define GIULIA_PCIE_%s_PA         0x%08xu' % (kind, ranges[i + 2]))
        w('#define GIULIA_PCIE_%s_SIZE        0x%08xu' % (kind, ranges[i + 5]))
    w('')
    w('#define GIULIA_PCIE_PERST_GPIO      %uu' % perst)
    w('#define GIULIA_PCIE_CLKREQ_GPIO     %uu' % clkreq)
    w('#define GIULIA_PCIE_WAKE_GPIO       %uu' % wake)
    w('#define GIULIA_CNSS_WLAN_EN_GPIO    %uu' % wlan_en)
    w('#define GIULIA_CNSS_BT_EN_GPIO      %uu' % bt_en)
    w('#define GIULIA_CNSS_SW_CTRL_GPIO    %uu' % sw_ctrl)
    w('#define GIULIA_CNSS_WLAN_SW_GPIO    %uu' % wlan_sw_ctrl)
    w('#define GIULIA_CNSS_QMP_MBOX        %uu' % words(cnss['qcom,qmp'])[0])
    w('')
    w('#define GIULIA_PCIE_PHY_STATUS      0x%03xu /* bit 6 = PHY ready */' % words(pcie['qcom,phy-status-offset'])[0])
    w('#define GIULIA_PCIE_PHY_STATUS_BIT  %uu' % words(pcie['qcom,phy-status-bit'])[0])
    w('#define GIULIA_PCIE_PHY_PWRDOWN     0x%03xu' % words(pcie['qcom,phy-power-down-offset'])[0])
    w('#define GIULIA_PCIE_EP_LATENCY_MS   %uu' % words(pcie['qcom,ep-latency'])[0])
    w('#define GIULIA_PCIE_SMMU_SID_BASE   0x%04xu' % words(pcie['qcom,smmu-sid-base'])[0])
    w('')
    w('struct giulia_pcie_phy_step {')
    w('\tuint32_t offset;')
    w('\tuint32_t value;')
    w('\tuint32_t delay_us;')
    w('};')
    w('')
    w('/* %d steps from the vendor qcom,phy-sequence. */' % (len(phy_seq) // 3))
    w('static const struct giulia_pcie_phy_step giulia_pcie_phy[] = {')
    for i in range(0, len(phy_seq), 3):
        w('\t{ 0x%03x, 0x%02x, %u },' % (phy_seq[i], phy_seq[i + 1], phy_seq[i + 2]))
    w('};')
    w('')
    w('/* qcom,pcie-sm-seq: written to the SM window before link training. */')
    w('static const uint32_t giulia_pcie_sm_seq[] = {')
    for i in range(0, len(sm_seq), 8):
        w('\t' + ', '.join('0x%08x' % v for v in sm_seq[i:i + 8]) + ',')
    w('};')
    w('')
    w('#define GIULIA_PCIE_SM_START_OFFSET 0x%04xu' % words(pcie['qcom,pcie-sm-start-offset'])[0])
    w('#define GIULIA_PCIE_SM_BRANCH_OFFSET 0x%04xu' % words(pcie['qcom,pcie-sm-branch-offset'])[0])
    w('static const uint32_t giulia_pcie_sm_branch[] = {')
    w('\t' + ', '.join('0x%x' % v for v in sm_branch) + ',')
    w('};')
    w('')
    w('/* Linux drivers/clk/qcom/gcc-sm8650.c: PCIe0 branches, GDSCs and resets. */')
    w('#define GIULIA_GCC_PCIE0_GDSCR        0x6b004u /* bit 0 on, bit 31 status */')
    w('#define GIULIA_GCC_PCIE0_PHY_GDSCR    0x6c000u')
    w('#define GIULIA_GCC_PCIE_VOTE          0x52008u /* bits 0-5: slv,mstr,ahb,aux,pipe,q2a */')
    w('#define GIULIA_GCC_PCIE0_SLV_HALT     0x6b020u')
    w('#define GIULIA_GCC_PCIE0_MSTR_HALT    0x6b02cu')
    w('#define GIULIA_GCC_PCIE0_AHB_HALT     0x6b038u')
    w('#define GIULIA_GCC_PCIE0_AUX_HALT     0x6b03cu')
    w('#define GIULIA_GCC_PCIE0_Q2A_HALT     0x6b01cu')
    w('#define GIULIA_GCC_PCIE0_PIPE_HALT    0x6b048u')
    w('#define GIULIA_GCC_PCIE0_CORE_BCR     0x6b000u')
    w('#define GIULIA_GCC_PCIE0_PHY_BCR      0x6c01cu')
    w('')
    w('#endif /* GIULIA_PCIE_REGS_H */')

    text = '\n'.join(out) + '\n'
    with open(args.out, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(text)
    print('wrote %s (%d bytes, %d phy steps, %d sm words)' %
          (args.out, len(text), len(phy_seq) // 3, len(sm_seq)), file=sys.stderr)


if __name__ == '__main__':
    main()
