# esp-ndf top level. Applications are built from their own folder (see docs/TESTING.md):
#     cd examples/system/sysinfo && make flash          cd components/drivers/spi/test_apps/spi_unit && make test
# Targets here work on many applications at once.
.PHONY: help list test test-all build-all security-test clean
help:
	@echo "make list                    test applications (kind, board)"
	@echo "make test [T=path] [KIND=unit] [BOARD=devkit]   flash + run + judge test apps below T (default: everything)"
	@echo "make build-all               build every application"
	@echo "make security-test           host fuzzers + OTA signature check"
	@echo "make clean                   remove every build folder"
list:
	python3 tools/ndf_test.py list $(T) --kind $(or $(KIND),all)
test:
	python3 tools/ndf_test.py run $(T) --kind $(or $(KIND),unit) $(if $(BOARD),--board $(BOARD))
build-all:
	python3 tools/build_all.py
security-test:
	tools/security_test.sh
clean:
	find . -name build -type d -prune -not -path './.git/*' -exec rm -rf {} + ; find . -name build-noaccel -type d -prune -exec rm -rf {} +
