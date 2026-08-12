
# pigzpp technical report — build with Tectonic
#
#   make pdf     Build build/main.pdf (installs Tectonic via setup.sh if missing)
#   make plots   Regenerate plots/*.pdf from plots/data/*.tsv (matplotlib)
#   make setup   Install Tectonic into ~/.local/bin
#   make watch   Rebuild on save (Tectonic watch mode)
#   make clean   Remove build artifacts

MAIN           := main.tex
PDF            := build/main.pdf
TECTONIC_FLAGS := --outdir build --keep-intermediates --keep-logs
PLOT_FILES     := plots/cli.pdf \
	plots/language_python.pdf plots/language_go.pdf plots/language_rust.pdf \
	plots/docker.pdf plots/wasm_single.pdf plots/wasm_scaling.pdf \
	plots/png.pdf plots/zip.pdf plots/decompression.pdf \
	plots/native_scaling.pdf plots/robustness.pdf

.PHONY: pdf plots setup watch clean

# Some prebuilt Tectonic Linux binaries require a newer glibc than the host
# provides (e.g. the GitHub gnu build needs glibc >= 2.36, which Ubuntu 22.04
# lacks). The conda-forge build is compiled against glibc 2.16 and runs anywhere.
# The recipe below probes candidates and uses the first that actually executes.
pdf: $(MAIN)
	@mkdir -p build; \
	if [ ! -f fonts/LibertinusSerif-Regular.otf ] || [ ! -f acl_natbib.bst ]; then \
	  echo "==> Fonts or bib style missing; fetching via setup.sh"; ./setup.sh >&2; \
	fi; \
	missing_plot=; for p in $(PLOT_FILES); do \
	  if [ ! -f "$$p" ]; then missing_plot=1; break; fi; \
	done; \
	if [ -n "$$missing_plot" ]; then \
	  echo "==> Plots missing; generating from plots/data"; $(MAKE) plots >&2; \
	fi; \
	find_tec() { for t in tectonic "$$HOME/.local/bin/tectonic" "$$CONDA_PREFIX/bin/tectonic" "$$HOME/mambaforge/bin/tectonic" "$$HOME/miniforge3/bin/tectonic" "$$HOME/miniconda3/bin/tectonic"; do \
	    command -v "$$t" >/dev/null 2>&1 && "$$t" --version >/dev/null 2>&1 && { echo "$$t"; return 0; }; \
	  done; return 1; }; \
	tec=$$(find_tec) || { echo "==> No working Tectonic found; running setup.sh"; ./setup.sh >&2; tec=$$(find_tec); }; \
	if [ -z "$$tec" ]; then echo "error: no working Tectonic found (see setup.sh)" >&2; exit 1; fi; \
	echo "==> Using $$tec"; \
	"$$tec" $(MAIN) $(TECTONIC_FLAGS) && echo "Built $(PDF)"

setup:
	./setup.sh

# Regenerate every figure from the checked-in benchmark data (plots/data/*.tsv).
# Requires matplotlib, pandas, and seaborn.
plots:
	python3 plots/make_plots.py

watch:
	@mkdir -p build; \
	tec=$${TECTONIC:-tectonic}; \
	command -v "$$tec" >/dev/null 2>&1 || tec="$$HOME/mambaforge/bin/tectonic"; \
	"$$tec" -X watch

clean:
	rm -rf build