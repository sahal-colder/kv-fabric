# SPDK physical NVMe validation

The reviewed 2026-11-28 measurements use a Samsung MZVLQ512HALU NVMe,
SPDK PCIe bdev/VFIO and an RTX 4060 Laptop GPU. CPU and GPU recovery each
have three warmups and 30 measured samples of 32 x 512 KiB (16 MiB).

- `summary.json`: exact statistics, timing scope, validation counts and source hashes.
- `cpu.csv`, `cuda.csv`: all 60 measured samples, with verified byte counts.
- `validation.txt`: sanitized validation output from the isolated publication checkout.
- [Full validation report](../../SPDK-NVME-VALIDATION.md): deployment, addressing,
  backup/restore and limitations. P50 GPU recovery is 18.95 ms; this is an
  independent storage benchmark with CRC, Host staging and H2D, not vLLM TTFT.

Hardware validation is explicit and is never registered in CTest. First identify
an exclusively owned scratch region, save its exact original bytes to a durable
private backup while it is still accessible, then configure VFIO for that disk.
The following example uses the region and bdev names from this validation:

```bash
python3 scripts/test_spdk_nvme.py \
  --build build-cuda-release --config /etc/kvfabric/spdk-samsung.json \
  --bdev Samsungn1 --offset 1073741824 --capacity 134217728 \
  --backup /private/original-scratch.bin --out /private/new-spdk-validation \
  --warmup 3 --rounds 30 --inference
```

`raw` verifies that the supplied backup matches the device region before
writing. The runner restores and reads back the original scratch bytes after
the tiered and optional inference tests, including on their failure paths.
A mismatched backup aborts before these tests and does not trigger an external
restore. Keep the private backup until validation and restoration are complete.
The runner uses CUDA for its GPU test; build with SPDK and CUDA enabled.

No disk contents, device serial numbers, credentials, private resumes, models,
host paths or machine environment dumps are published here. Original hardware
measurements remain unchanged. The publication checkout is built independently;
its regression output is recorded separately from the original performance run.
