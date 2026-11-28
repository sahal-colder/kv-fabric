#!/usr/bin/env python3
"""Publish only complete, restored, same-source A/B/C runs, plus scientific plots."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def load(path):
    return json.loads(path.read_text())


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    suite = load(directory/'suite.json')
    require(all(p['returncode'] == 0 for p in suite['phases'] if p['required']), 'required phase failed')
    runs = {name: load(directory/name/'run.json') for name in ['raw', 'migration', 'inference']}
    for name, run in runs.items():
        require(not run.get('error') and run.get('finished'), name+' incomplete')
        require(all(c['matches_original'] for c in run['checks']['after']), name+' restoration failed')
        require(run['final_state']['driver'] == run['initial_state']['driver'], name+' driver mismatch')
        require(run['final_hugepages_2m'] == run['original_hugepages_2m'], name+' hugepages mismatch')
        require(run['final_nvme_poll_queues'] == run['original_nvme_poll_queues'], name+' poll queues mismatch')
        source_root = directory/name/run['source_archive'] if run.get('source_archive') else ROOT
        for path, digest in run['source_sha256'].items():
            require(hashlib.sha256((source_root/path).read_bytes()).hexdigest() == digest, 'measured source changed: '+path)
        binary = {'raw': 'kvf_async_bench', 'migration': 'kvf_migration_bench', 'inference': 'miniinfer'}[name]
        require(hashlib.sha256((ROOT/'build-spdk/bin'/binary).read_bytes()).hexdigest() == run['executable_sha256'][binary], 'used binary changed: '+binary)
    raw = load(directory/'raw/summary.json')['groups']
    migration = load(directory/'migration/summary.json')['groups']
    inference = load(directory/'inference/summary.json')['groups']
    require(len(raw) == 490 and all(r['samples'] == 60 for r in raw), 'incomplete raw matrix')
    require(len(migration) == 280 and all(r['samples'] == 60 for r in migration), 'incomplete migration matrix')
    require(len(inference) == 8 and all(r['samples'] == 60 for r in inference), 'incomplete inference matrix')
    for layer in runs:
        require(all(c.get('returncode') == 0 for c in runs[layer]['commands']), layer+' command failed')
    env = load(directory/'raw/environment.json')
    source_paths = subprocess.check_output(['rg', '--files', 'src', 'include', 'tools', 'scripts', 'tests', 'config', 'cmake',
                                            'third_party/infer-io', 'third_party/NtyCo'], cwd=ROOT, text=True).splitlines()
    sources = {p: hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in source_paths
               if Path(p).suffix in {'.c', '.h', '.cu', '.py', '.sh', '.cmake', '.json', '.conf'} or Path(p).name in {'CMakeLists.txt', 'Makefile'}}
    for p in ['CMakeLists.txt', 'Makefile']:
        sources[p] = hashlib.sha256((ROOT/p).read_bytes()).hexdigest()
    (directory/'source-sha256.json').write_text(json.dumps(sources, indent=2)+'\n')
    rel = directory.relative_to(ROOT/'docs')
    lines = ['# SPDK / io_uring 同盘性能与可靠性报告', '',
             '日期：2026-11-28。以下均为生产实现的真实测量，不使用历史 backend_compare 的独立 SPDK 快路。Raw 测量后只优化了其不链接的记录层；Raw 源码已归档，实测 Raw 二进制 SHA 与最终编译产物完全一致。迁移及推理使用最终记录层重新测量。', '',
             '## 环境、数据语义与保护', '',
             f"设备：{runs['raw']['manifest']['serial']}，PCI {runs['raw']['manifest']['pci']}，同一 Samsung 512 GB NVMe。",
             'io_uring 内核 NVMe 与 SPDK VFIO 顺序占用，提交 CPU=4、I/O owner CPU=2。完整环境、实际 SPDK JSON、所有命令和源码/可执行文件 SHA 均保留。',
             'Raw 区域 1 GiB 起、128 MiB；迁移区域为 129 MiB，额外 1 MiB 先单独持久备份。所有区域恢复 SHA-256 一致，驱动、服务、大页和 NVMe poll_queues 原值恢复。',
             'Raw 为相同的非持久完成语义；O_DIRECT、固定文件、注册缓冲。Raw CRC/严格字节验证单独计时，迁移路径 CRC 不关闭。每配置 3 轮预热、30 轮正式，两个交替后端顺序批次，全部原始样本保留。',
             '每轮操作数为 max(QD, min(512, 16 MiB / block_size)) 并按 QD 补齐；128 MiB 小区域及短窗口不能代表全盘稳态写入。', '',
             '## A：Raw 后端', '',
             '完整 7 大小 × 7 QD × 5 负载 × 2 后端 × 2 顺序批次，共 980 进程配置、29,400 正式轮次。',
             '下表为每配置 60 轮中位数，尾延迟来自其全部 I/O 实际完成样本。没有选最好一轮。', '',
             '| 大小/负载 | QD | 后端 | IOPS | MiB/s | P99 µs | cycles/I/O | CPU % |',
             '|---|---:|---|---:|---:|---:|---:|---:|']
    selected = [(4096, 'rand-read', q) for q in [1, 32, 128]] + [(524288, w, q) for w in ['seq-read', 'seq-write'] for q in [1, 32, 128]]
    for size, work, qd in selected:
        for backend in ['uring', 'spdk']:
            r = next(r for r in raw if (r['bytes'], r['workload'], r['qd'], r['backend']) == (size, work, qd, backend))
            cycles = '不可用' if r['cpu_cycles_per_io'] is None else f"{r['cpu_cycles_per_io']:.0f}"
            lines.append(f"| {size//1024} KiB {work} | {qd} | {backend} | {r['iops_median']:.0f} | {r['mib_s_median']:.1f} | {r['p99_us']:.1f} | {cycles} | {r['cpu_utilization_percent']:.1f} |")
    lines += ['', '| 后端 | 最大真实 bdev/SQE 在途数 |', '|---|---:|']
    for backend in ['uring', 'spdk']:
        lines.append(f"| {backend} | {max(r['actual_max_inflight'] for r in raw if r['backend'] == backend)} |")
    lines += ['', 'Queue Wait 为提交排队到 dispatch，copy 单独记录。CPU 为计时 I/O 窗口内进程 CPU；PMU cycles 覆盖该进程已存在的线程，包含短计数器控制边界，不是 TSC 估算，不涵盖外部 IRQ/kworker 或 SQPOLL 内核线程的全部消耗。',
              '配对报告保留每轮及每进程批次比值；同进程内轮次相关，只有两个独立顺序批次，不给显著性或生产容量承诺。', '',
              '## B：KV Page 迁移', '',
              '全部为 512 KiB Page，批量 1/32/64/128/256，运行上限 1/4/8/16/32/64/128，CPU/GPU Spill 与 Recovery。280 个进程配置。以下展示 128 Page 的 GPU 迁移；完整 280 个方向分组和累计阶段计时见汇总。', '',
              '| 方向 | 并发上限 | 后端 | GiB/s 中位数 | P99 batch ms | 实测最大运行数 |', '|---|---:|---|---:|---:|---:|']
    for direction in ['spill', 'recovery']:
        for qd in [1, 16, 128]:
            for backend in ['uring', 'spdk']:
                r = next(r for r in migration if (r['device'], r['pages'], r['concurrency'], r['direction'], r['backend']) == ('gpu', 128, qd, direction, backend))
                lines.append(f"| {direction} | {qd} | {backend} | {r['gib_s_median']:.3f} | {r['p99_batch_ms']:.3f} | {r['max_running']} |")
    lines += ['', '阶段数据分别列出 SSD 等待、CRC、Host memcpy、D2H、H2D；并发阶段累计 wall time 会重叠，不能简单相加还原 elapsed。没有采集精确 GPU/NVMe 时间线重叠比例，也不把 batch P99 冒充单 Page P99。', '',
              '## C：CUDA 推理', '',
              '真实运行 Nano-VLLM 的随机权重 dim=32、2 层 CUDA fixture，逐轮 Token IDs 与无 SSD 基线完全一致。它是功能与回源等待证据，不是生产大模型性能结论。', '',
              '| 缓存情况 | 后端 | TTFT P50 ms | TTFT P99 ms | 同步恢复等待中位 ms |', '|---|---|---:|---:|---:|']
    for case in ['cold-A', 'ram-hot-A', 'cold-B', 'ssd-return-A']:
        for backend in ['uring', 'spdk']:
            r = next(r for r in inference if (r['cache_case'], r['backend']) == (case, backend))
            lines.append(f"| {case} | {backend} | {r['p50_ttft_ms']:.3f} | {r['p99_ttft_ms']:.3f} | {r['restore_wait_ms_median']:.3f} |")
    lines += ['', '恢复等待是推理调度同步等待 SSD→KV 恢复，不是 GPU 硬件 idle counter。A/B/C 不混算加速比。', '',
              '## 回归、限制与复现', '',
              'Release/CUDA/SPDK 48 项；ASan+UBSan/CUDA/SPDK 48 项（ASAN_OPTIONS=protect_shadow_gap=0）；TSan/SPDK 43 项（setarch -R），均通过。测试只静态注入 SPDK 资源不足/移除事件，未实际热拔盘。',
              '网络 Reactor/Proactor/NtyCo 独立回归通过；网络源码 SHA 保持，三种执行模型及复制入口完整保留。本机无 RDMA 设备，未声称物理 RDMA 往返通过。',
              '生产未启用共享 CUDA/SPDK DMA Pool；大页内存注册与 CUDA 往返原型已通过。多 Reactor、生产模型级并发 Prefetch/TTFT、掉电原子恢复及实际热移除仍未验证。旧 buffered API 的 MIFSSD1 容量兼容路径保留，对齐两后端为 MIFSSD3。', '',
              f'- [Raw CSV/JSON]({rel}/raw/summary.csv)，[配对及全部分位数]({rel}/raw/summary.json)。',
              f'- [迁移 CSV]({rel}/migration/summary.csv)，[阶段/配对 JSON]({rel}/migration/summary.json)。',
              f'- [推理 CSV]({rel}/inference/summary.csv)，[推理 JSON]({rel}/inference/summary.json)。',
              f'- [环境]({rel}/raw/environment.json)，[源码 SHA-256]({rel}/source-sha256.json)。',
              f'- 每个目录的 run.json 含完整命令、备份 SHA、结束状态、源码及可执行文件 SHA；*-io.csv 保留每个原始 I/O，*-rounds.csv 包含预热与测量。', '',
              '```bash',
              'sudo python3 scripts/run_spdk_async_suite.py --skip-profiles --out /absolute/new/result',
              'python3 scripts/render_spdk_async_report.py /absolute/new/result',
              '# 此 runner 锁定本机已核验序列号及备份清单；其他机器先建立其专用设备/区域清单。',
              '```', '']
    (ROOT/'docs/SPDK-ASYNC-IO-BENCHMARK.md').write_text('\n'.join(lines))
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print('report generated; matplotlib unavailable for optional plots')
        return
    fig, axes = plt.subplots(2, 2, figsize=(12, 8), constrained_layout=True)
    for backend, color in [('uring', '#d97706'), ('spdk', '#2563eb')]:
        rows = sorted([r for r in raw if r['backend'] == backend and r['bytes'] == 4096 and r['workload'] == 'rand-read'], key=lambda r: r['qd'])
        axes[0, 0].plot([r['qd'] for r in rows], [r['iops_median']/1000 for r in rows], 'o-', color=color, label=backend)
        axes[1, 0].plot([r['qd'] for r in rows], [r['p99_us'] for r in rows], 'o-', color=color, label=backend)
        rows = sorted([r for r in raw if r['backend'] == backend and r['bytes'] == 524288 and r['workload'] == 'seq-read'], key=lambda r: r['qd'])
        axes[0, 1].plot([r['qd'] for r in rows], [r['mib_s_median']/1024 for r in rows], 'o-', color=color, label=backend)
        rows = sorted([r for r in migration if r['backend'] == backend and r['device'] == 'gpu' and r['pages'] == 128 and r['direction'] == 'recovery'], key=lambda r: r['concurrency'])
        axes[1, 1].plot([r['concurrency'] for r in rows], [r['gib_s_median'] for r in rows], 'o-', color=color, label=backend)
    titles = ['A: 4 KiB random read', 'A: 512 KiB sequential read', 'A: 4 KiB random read P99', 'B: 128 GPU Pages recovery (512 KiB/Page)']
    labels = ['kIOPS', 'GiB/s', 'microseconds', 'GiB/s']
    for ax, title, label in zip(axes.flat, titles, labels):
        ax.set_title(title)
        ax.set_xlabel('Queue depth / migration concurrency')
        ax.set_ylabel(label)
        ax.set_xscale('log', base=2)
        ax.set_xticks([1, 4, 8, 16, 32, 64, 128], ['1', '4', '8', '16', '32', '64', '128'])
        ax.grid(alpha=.2)
        ax.legend()
    fig.savefig(directory/'overview.png', dpi=170)
    fig.savefig(directory/'overview.svg')
    print('verified report, source manifest and plots generated')


if __name__ == '__main__':
    main()
