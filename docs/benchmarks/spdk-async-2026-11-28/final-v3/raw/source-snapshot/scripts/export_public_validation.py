#!/usr/bin/env python3
"""Export measured vLLM cells and synthetic output tokens without host metadata.

This is an allowlist, not a search/replace redactor. Raw environment files,
commands, service logs, local paths, request IDs and model files are excluded.
The caller must check that the archive contains synthetic, publishable inputs.
"""
import argparse
import json
from pathlib import Path

CELL_FIELDS = {'matrix','mode','concurrency','requests','prompt_tokens','warmup_requests',
 'output_tokens','token_id_verified_requests','gpu_reset_between_batches','throughput_scope',
 'req_s','output_tokens_s','ttft_ms','tpot_ms','itl_ms','actual_hit_rates',
 'kvfabric_load_failures','kvfabric_save_failures','kvfabric_save_skipped','kvfabric_io_mode'}
REQUEST_FIELDS = {'prompt_tokens','output_ids','prompt_sha256','ttft_ms','tpot_ms','itl_ms','output_chunk_sizes'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--expected-requests', type=int, required=True)
    parser.add_argument('--date', required=True)
    parser.add_argument('--source-version', required=True)
    parser.add_argument('--synthetic-inputs-confirmed', action='store_true', required=True)
    args = parser.parse_args()
    source = json.loads((args.archive/'matrix-summary.json').read_text())
    cells = [{k:v for k,v in row.items() if k in CELL_FIELDS} for row in source['cells']]
    requests = []
    for folder in sorted(args.archive.glob('matrix-*')):
        if not folder.is_dir():
            continue
        for path in sorted(folder.glob('*/requests.jsonl')):
            for line in path.read_text().splitlines():
                row = json.loads(line)
                requests.append({'matrix':folder.name,'mode':path.parent.name,
                    **{k:v for k,v in row.items() if k in REQUEST_FIELDS}})
    if len(requests) != args.expected_requests or sum(r['requests'] for r in cells) != len(requests):
        raise ValueError('Measured cell/request counts do not reconcile')
    args.out.mkdir(parents=True, exist_ok=False)
    metadata = {'date':args.date,'source_version':args.source_version,
                'note':'Synthetic token inputs; host identity and paths omitted. n=15 nearest-rank P95/P99 are maxima.'}
    (args.out/'matrix.json').write_text(json.dumps({**metadata,'cells':cells},indent=2))
    (args.out/'tokens.json').write_text(json.dumps({**metadata,'requests':requests},indent=2))
    print('Exported',len(cells),'measured cells and',len(requests),'synthetic requests')


if __name__ == '__main__':
    main()
