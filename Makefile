# Top-level Makefile: builds and checks every lab.
# Students normally work inside one lab directory and run 'make' there.

LABS := $(sort $(wildcard lab[0-9][0-9]-*))
COMPOSE := docker compose -f docker/compose.yaml

all:
	@for d in $(LABS); do $(MAKE) -C $$d all || exit 1; done

# Build with -Werror, then run every lab's short functional check.
smoke:
	@for d in $(LABS); do $(MAKE) -C $$d WERROR=1 all || exit 1; done
	@for d in $(LABS); do echo "=== $$d ==="; $(MAKE) -s -C $$d smoke || exit 1; done
	@echo "smoke: all labs OK"

clean:
	@for d in $(LABS); do $(MAKE) -C $$d clean; done

# Local Debian 13 environment (mirror of the lab machine, not PREEMPT_RT).
docker-build:
	$(COMPOSE) build

docker-shell:
	$(COMPOSE) run --rm lab

docker-smoke:
	$(COMPOSE) run --rm -T lab make smoke

.PHONY: all smoke clean docker-build docker-shell docker-smoke
