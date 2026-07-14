# Benchmarking

Sweeps [h2load](https://nghttp2.org/documentation/h2load.1.html) over a grid of `-c` (number of
clients/connections) and `-m` (max concurrent streams per client) values against an already-running
`server_main`, and renders the resulting requests/s as a heatmap and a line chart.

## Usage

Start the server, then in another shell:

```sh
./build/server_main            # or whatever port/config server_main takes
cd benchmark
./h2load_matrix.sh
gnuplot plot.gnuplot
```

This produces `heatmap.png` (req/s over the full `-c`/`-m` grid) and `lines.png` (req/s vs `-m`, one
line per `-c` value). Both scripts must be run with `benchmark/` as the working directory, since all
file references (`results.dat`, `sweep_values.gnuplot`, the output PNGs) are relative paths.

`h2load_matrix.sh` env overrides:

| Var        | Default                          | Meaning                                      |
|------------|-----------------------------------|-----------------------------------------------|
| `URL`      | `http://localhost:8080/echo`     | target URL passed to h2load                   |
| `DURATION` | `1s`                              | measured time per `-c`/`-m` cell (h2load `-D`)|
| `WARMUP`   | `500ms`                           | warm-up time before measurement starts (h2load `--warm-up-time`) |
| `DATA`     | *(unset)*                        | file passed as h2load's `-d`, e.g. an **absolute** path to `test/data/1k` to POST a fixed body |

`DATA` must be an absolute path (or relative to `benchmark/`, not the repo root) since h2load resolves
it relative to its own working directory.

## Why duration-based, not request-count-based

h2load can be run either for a fixed request count (`-n`) or for a fixed duration (`-D`, with
`--warm-up-time` for a warm-up period beforehand; the two are mutually exclusive with `-r`, and `-n`
is ignored once `-D` is set). This sweep uses `-D`/`--warm-up-time`: with a slow scenario (e.g. a
`-d` payload at low `c`*`m`, where per-request latency can dominate), a fixed request count can take
minutes to complete for a single cell since h2load runs until all requests finish regardless of
throughput. A fixed duration bounds every cell to roughly the same wall-clock time no matter how slow
that cell turns out to be, so the whole grid finishes in predictable time.

## Files

- `h2load_matrix.sh` — the sweep driver and single source of truth for the `-c`/`-m` value set
  (the `VALUES` array). Writes `results.dat` (measurements) and `sweep_values.gnuplot` (the same
  value set, for `plot.gnuplot` to `load` rather than duplicate by hand).
- `plot.gnuplot` — renders `results.dat` into `heatmap.png`/`lines.png`.
- `results.dat`, `sweep_values.gnuplot`, `*.png`, `h2load_matrix.log`, `sweep_progress.log` — generated
  output, gitignored.
