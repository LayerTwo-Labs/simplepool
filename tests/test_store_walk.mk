# White-box: includes src/store.c as source (fault injection on sqlite3_step),
# so it does NOT link store.c the way the other suites do.
test_store_walk_bin = build/test_store_walk
$(test_store_walk_bin): tests/test_store_walk.c src/store.c src/log.c
	mkdir -p build
	$(CC) $(CFLAGS) -Wno-unused-function -Isrc -o $(test_store_walk_bin) tests/test_store_walk.c src/log.c -lsqlite3 -lpthread
test_store_walk: $(test_store_walk_bin)
	./$(test_store_walk_bin)
