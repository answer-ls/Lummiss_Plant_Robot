"""解析首次H264前布局；只用实测block计算候选，不虚构owner。"""
import argparse
import re
import subprocess
from pathlib import Path


def analyze(text, target=0x4FF861C0):
    blocks = [dict(heap=int(h, 16), end=int(e, 16), ptr=int(p, 16), size=int(s), used=int(u))
              for h, e, p, s, u in re.findall(
                  r"BLOCK heap=(0x\w+) end=(0x\w+) ptr=(0x\w+) size=(\d+) used=([01])", text)]
    # 边界随链接改变：先匹配起点，否则选覆盖原调查地址的实际region。
    heaps = sorted({(b['heap'], b['end']) for b in blocks})
    region = next((h for h in heaps if h[0] == target), None)
    region = region or next((h for h in heaps if h[0] <= target < h[1]), None)
    selected = sorted((b for b in blocks if region and b['heap'] == region[0]), key=lambda b: b['ptr'])
    callers = {}
    for ptr, depth, pc in re.findall(r"CALLER block=(0x\w+) depth=(\d+) pc=(0x\w+)", text):
        callers.setdefault(int(ptr, 16), []).append(pc)
    candidates = []
    for i, b in enumerate(selected):
        if not b['used']:
            continue
        left = selected[i-1]['size'] if i and not selected[i-1]['used'] else 0
        right = selected[i+1]['size'] if i+1 < len(selected) and not selected[i+1]['used'] else 0
        if left or right:
            candidates.append(dict(**b, left=left, right=right, merged=left+b['size']+right,
                                   callers=callers.get(b['ptr'], [])))
    return region, selected, sorted(candidates, key=lambda c: c['merged'], reverse=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('log', type=Path)
    p.add_argument('--elf', type=Path)
    p.add_argument('--addr2line', default='riscv32-esp-elf-addr2line')
    p.add_argument('--region', type=lambda s: int(s, 0), default=0x4FF861C0)
    args = p.parse_args()
    text = args.log.read_text(encoding='utf-8', errors='replace')
    region, blocks, candidates = analyze(text, args.region)
    out = ['# H264 INTERNAL 实测布局', '', f'源日志：{args.log.resolve()}',
           f'实际region：{region}', '',
           '以下合并大小为相邻载荷之和，不包含回收的分配器元数据；对齐和并发变化仍以实际申请为准。',
           '没有caller的块保持未知；ISR、启动前分配、trace溢出以及快照竞争均可能导致缺失。', '',
           '## 完整布局', '', '| ADDRESS | SIZE | STATE |', '|---|---:|---|']
    out += [f"| 0x{b['ptr']:08x} | {b['size']} | {'ALLOCATED' if b['used'] else 'FREE'} |" for b in blocks]
    out += ['', '## Splitter候选（按单块释放后连续空间排序）', '',
            '| ADDRESS | SIZE | LEFT FREE | RIGHT FREE | 合并估计 | >=92224 | caller |', '|---|---:|---:|---:|---:|---|---|']
    for c in candidates:
        owner = ', '.join(c['callers']) or '未知'
        if args.elf and c['callers']:
            owner = subprocess.check_output([args.addr2line, '-pfiaC', '-e', str(args.elf), *c['callers']], text=True).strip().replace('\n', '<br>')
        out.append(f"| 0x{c['ptr']:08x} | {c['size']} | {c['left']} | {c['right']} | {c['merged']} | {c['merged']>=92224} | {owner} |")
    if not blocks:
        out += ['', '**没有完整block快照，不能根据summary生成布局。**']
    out += ['', '## 迁移/延后/释放判定', '',
            '上述caller仅用于定位源码；逐项核对调用链、DMA/ISR/cache约束及生命周期后，才能判定迁PSRAM、延后创建或RTC释放。当前均未确认。',
            '若没有单块候选达到阈值，不能声称释放一个对象即可解决；需继续分析连续多块并保留所有存活块约束。',
            '', '## 完整性标志', '', '```',
            *[s for s in text.splitlines() if 'LAYOUT_BEGIN' in s or 'TRACE_END' in s or 'LAYOUT_END' in s], '```']
    dest = Path(__file__).resolve().parents[3] / 'logs' / 'h264_fragment' / (args.log.stem + '_H264_INTERNAL_FRAGMENTATION.md')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text('\n'.join(out)+'\n', encoding='utf-8')
    print(dest)


if __name__ == '__main__':
    main()
