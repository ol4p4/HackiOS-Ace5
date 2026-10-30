#!/usr/bin/env python3
"""The hardware facts a driver port needs, out of giulia's own device tree.

Every driver port starts by replacing another phone's constants with this
phone's: which bus a chip sits on, at what address, on which SPI/GPIO, with
which firmware name. This prints those facts grouped by subsystem, so
`docs/ACE5-HW.md` can be regenerated instead of hand-copied.

    python3 tools/ace5/hw-facts.py dtb/giulia.dtb --out docs/ACE5-HW.md

Identifying properties (serials, MAC addresses, bootargs) are dropped on the
way through: this document goes into Git, the device tree does not have to.
"""
import argparse
import struct
import sys

SECTIONS = [
    ('Wi-Fi / connectivity',
     ('wifi', 'wlan', 'wcn', 'cnss', 'icnss', 'qca', 'pcie', 'pcie@', 'bluetooth'),
     ('qcom,qmp', 'qcom,wlan', 'qcom,bt-en-gpio', 'qcom,sw-ctrl-gpio', 'qcom,stream-id-range',
      'qcom,iova-width', 'qcom,num-qtb-ports', 'qcom,rc-num', 'qcom,firmware-name',
      'qcom,vdd-', 'qcom,vreg_pdc_map', 'qcom,pmu_vreg_map', 'qcom,pdc_init_table',
      # PCIe host-controller facts the M1/M2 steps stand on: register bases,
      # the vendor PHY and state-machine sequences, GPIOs, clock/reset names,
      # the RID->SID map and the IOVA windows.
      'reg-names', 'clocks', 'clock-names', 'clock-frequency', 'clock-suppressible',
      'power-domains', 'power-domain-names', 'resets', 'reset-names',
      'perst-gpio', 'wake-gpio', 'qcom,pcie-clkreq-gpio',
      'ranges', 'iommu-map', 'msi-map', 'qcom,smmu-sid-base', 'qcom,slv-addr-space-size',
      'qcom,phy-', 'qcom,pcie-sm-', 'qcom,dbi-debug-reg', 'qcom,parf-debug-reg',
      'qcom,aux-clk-freq', 'qcom,boot-option', 'qcom,pcie-phy-ver',
      'qcom,num-parf-testbus-sel', 'qcom,ep-latency', 'qcom,l1-2-', 'qcom,drv-',
      'qcom,config-recovery', 'qcom,same-dt-multi-dev', 'qcom,bus-bw-cfg',
      'qcom,icc-path-count', 'supported-ids', 'qcom,dev-id', 'memory-region',
      'wlan-en-gpio', 'qcom,iommu-', 'use-nv-mac', 'use-pm-domain',
      'mpm_wake_set_gpios', 'cnss-enable-self-recovery', 'interrupt-names', 'gdsc-')),
    ('Display / backlight',
     ('mdss', 'dsi', 'backlight', 'wled', 'pwm', 'panel'),
     ('qcom,mdss-dsi-panel-name', 'qcom,mdss-dsi-panel-width', 'qcom,mdss-dsi-panel-height',
      'qcom,mdss-dsi-panel-framerate', 'qcom,mdss-dsi-panel-type', 'qcom,mdss-pref-prim-intf',
      'qcom,mdss-dsi-bl-', 'qcom,mdss-dsi-display-timings', 'qcom,dsi-phy-', 'qcom,mdss-dsi-t-clk',
      'qcom,panel-supply-entries', 'qcom,mdss-dsi-no-panel-on-read-support')),
    ('PMIC / GPIO / keys',
     ('spmi', 'pmic', 'pon', 'gpio', 'pinctrl', 'key', 'tlmm', 'pm8550', 'pmk', 'pmr', 'pm8010'),
     ('qcom,code', 'qcom,key-codes', 'qcom,resin-', 'qcom,pon-', 'qcom,hw-settable',
      'qcom,pmic-rev', 'qcom,pwrkey-', 'qcom,irq-gpio', 'qcom,gpio-',
      'gpios', 'linux,code', 'wakeup-source', 'interrupt-parent')),
    ('Battery / charger',
     ('battery', 'bms', 'gauge', 'bq27', 'charger', 'smb', 'power-supply', 'fg@'),
     ('qcom,fg-', 'qcom,battery-', 'qcom,chg-', 'qcom,step-', 'qcom,jeita-', 'qcom,usb-',
      'qcom,thermal-', 'qcom,dp-', 'qcom,hvdcp-', 'qcom,vbatt', 'qcom,qpnp-')),
    ('Buses',
     ('i2c', 'spi', 'geni', 'qup', 'slimbus'),
     ('qcom,geni-', 'qcom,i2c-', 'qcom,spi-', 'qcom,clk-freq-out', 'qcom,rt')),
    ('Audio',
     ('audio', 'wcd', 'codec', 'adsp', 'lpass', 'sia', 'cs35', 'swr', 'sound', 'macro'),
     ('qcom,model', 'qcom,codec-', 'qcom,adsp-', 'qcom,wsa-', 'qcom,wcd-', 'qcom,swr-',
      'qcom,sia', 'qcom,mi2s', 'qcom,audio-', 'qcom,msm-mi2s', 'qcom,tdm-')),
    ('Camera',
     ('camera', 'csiphy', 'csid', 'ife', 'camss', 'cci', 'eeprom', 'sensor'),
     ('qcom,csiphy-', 'qcom,csid-', 'qcom,ife-', 'qcom,cam-', 'qcom,cci-', 'qcom,src-clk')),
    ('Other SoC services',
     ('glink', 'smem', 'ipcc', 'rsc', 'aoss', 'cmd-db', 'mpss', 'ipa', 'pas'),
     ('qcom,pid', 'qcom,pas-id', 'qcom,firmware-name', 'qcom,smem', 'qcom,ipcc')),
]

DROP = ('serial', 'mac-address', 'local-mac-address', 'qcom,device-id', 'qcom,msm-id',
        'qcom,board-id', 'qcom,pmic-rev', 'label')

# Nodes that carry no fact a driver port can use: pinctrl leaves are one line per
# pad and their parent already names the pin group, and dcvs/interconnect rates are
# the vendor's bandwidth table, not a register.
SKIP_PATH = ('/config', '/mux', '/qcom,dcvs', '/interconnect@', '/qcom,cpu-llcc-ddr-bwmon')


def brief(text_value, limit=96):
    if len(text_value) > limit:
        return '<%d bytes>' % len(text_value)
    return text_value


# Register maps and SID/IOVA maps are the point of the PCIe rows; truncating them
# at the default width is what hid the DBI/ATu bases in the first place.
LIMITS = {'reg': 400, 'ranges': 400, 'iommu-map': 400, 'msi-map': 400,
          'qcom,vreg_pdc_map': 400, 'qcom,pmu_vreg_map': 400,
          'qcom,pcie-sm-seq': 400, 'qcom,pcie-sm-branch-seq': 400,
          'qcom,pcie-sm-debug': 400}


def string_list(value):
    """NUL-separated printable runs: *-names and the vreg/PDC tables."""
    if not value:
        return None
    try:
        as_text = value.decode('utf-8')
    except UnicodeDecodeError:
        return None
    if '\0' not in as_text or any(ord(ch) < 32 and ch != '\0' for ch in as_text):
        return None
    if ord(as_text[0]) < 32:  # a big-endian cell such as 0x5f, not a string
        return None
    parts = [part for part in as_text.split('\0') if part]
    if not parts or len(parts[0]) < 2:
        return None
    if not any(ch.isprintable() and not ch.isspace() for ch in parts[0]):
        return None
    return ' | '.join(parts)


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


def text(value):
    return value.rstrip(b'\0').decode('utf-8', 'replace')


def show(name, value):
    if name in ('compatible', 'status', 'model', 'qcom,firmware-name', 'qcom,board-id'):
        return ' '.join(text(value).split('\0')) if name == 'compatible' else text(value)
    listed = string_list(value)
    if listed is not None:
        return listed
    if len(value) and len(value) % 4 == 0:
        words = struct.unpack('>%dI' % (len(value) // 4), value)
        if name in ('reg', 'interrupts', 'interrupts-extended'):
            return ' '.join('%#x' % w for w in words)
        if all(w < 0x10000 for w in words) and len(words) <= 8:
            return ' '.join(str(w) for w in words)
        return ' '.join('%#x' % w for w in words)
    return value.hex()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dtb')
    ap.add_argument('--out', help='write markdown here instead of stdout')
    args = ap.parse_args()

    with open(args.dtb, 'rb') as handle:
        nodes = parse(handle.read())

    lines = []
    for title, keywords, wanted in SECTIONS:
        rows = []
        for path, props in sorted(nodes.items()):
            if path == '/' or any(skip in path for skip in SKIP_PATH):
                continue
            # One pinctrl group per pad is a thousand rows of noise; the driver node
            # itself names the group it wants, so only the controller is kept.
            if '/pinctrl@' in path and path.count('/') > 2:
                continue
            haystack = (path + ' ' + text(props.get('compatible', b''))).lower()
            if not any(key in haystack for key in keywords):
                continue
            keep = []
            if 'compatible' in props:
                keep.append('`%s`' % show('compatible', props['compatible']))
            for name in ('reg', 'interrupts'):
                if name in props:
                    keep.append('%s: `%s`' % (name, brief(show(name, props[name]),
                                                          LIMITS.get(name, 96))))
            for name in sorted(props):
                if name in ('compatible', 'reg', 'interrupts') or name in DROP:
                    continue
                if name.startswith(wanted):
                    keep.append('%s: `%s`' % (name, brief(show(name, props[name]),
                                                          LIMITS.get(name, 96))))
            rows.append((path, keep))
        lines.append('### %s\n' % title)
        if not rows:
            lines.append('_no matching nodes_\n')
            continue
        lines.append('| node | properties |')
        lines.append('|---|---|')
        for path, keep in rows[:40]:
            lines.append('| `%s` | %s |' % (path, '<br>'.join(keep) if keep else '—'))
        if len(rows) > 40:
            lines.append('| _%d more_ | |' % (len(rows) - 40))
        lines.append('')

    body = '\n'.join(lines)
    if args.out:
        # The document's top half is hand-written prose; only the appendix below
        # the first section heading is generated. Keep the prose when the target
        # already has it, so the documented command regenerates in place.
        marker = '### Wi-Fi / connectivity'
        try:
            with open(args.out, encoding='utf-8') as handle:
                old = handle.read()
        except FileNotFoundError:
            old = ''
        if marker in old:
            body = old.split(marker, 1)[0] + body
        with open(args.out, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(body)
        print('wrote %s (%d bytes)' % (args.out, len(body)), file=sys.stderr)
    else:
        print(body)


if __name__ == '__main__':
    main()
