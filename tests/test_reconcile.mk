build/test_reconcile: tests/test_reconcile.c src/reconcile.c src/store.c src/log.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_reconcile $^ -lsqlite3 -lpthread
