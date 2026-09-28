"""用本次固件ELF离线解码定向owner，不让串口监视器展开调用栈。"""
import argparse
import re
import subprocess
from pathlib import Path


def decode(text, elf, addr2line):
    rows = ['# reserve region owner实测', '', '仅按地址和调用栈归属，不按分配大小猜模块。', '']
    for line in text.splitlines():
        if not re.search(r'FRAG: (EDGE|GROUP|FOCUSED_DONE|FOCUSED_REGION|TRACE_END|LAYOUT_BEGIN|LARGEST|NEIGHBOR|CANDIDATE|BACKWARD_)', line):
            continue
        rows += ['```text', line, '```']
        m = re.search(r'pcs=([0-9a-fA-F,]+)\b', line)
        if m:
            pcs = ['0x'+pc for pc in m[1].split(',')]
            stack = subprocess.check_output([addr2line, '-pfiaC', '-e', str(elf), *pcs], text=True)
            rows += ['```text', stack.rstrip(), '```']
        elif 'pcs=unknown' in line:
            rows += ['owner未知：需要额外证据，不能推断为可释放。']
    return '\n'.join(rows)+'\n'


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('log', type=Path)
    p.add_argument('--elf', required=True, type=Path)
    p.add_argument('--addr2line', default='riscv32-esp-elf-addr2line')
    a = p.parse_args()
    result = decode(a.log.read_text(encoding='utf-8', errors='replace'), a.elf, a.addr2line)
    out = Path(__file__).resolve().parents[3]/'logs'/'h264_fragment'/(a.log.stem+'_owners.md')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(result, encoding='utf-8')
    print(out)
