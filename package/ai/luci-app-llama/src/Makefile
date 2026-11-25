# C 版 agent 框架 + 插件
#
#   make            编译框架 agent 和所有插件
#   make install    拷到 /opt/llama/agent/（板上直接跑）
#   make clean
#
# 板上是 gcc 8.4（musl），没有 cJSON/curl，所以框架自带最小 JSON 解析和
# 裸 socket HTTP —— 不依赖任何第三方库，-static 也能编。

CC      ?= gcc
# -U_FORTIFY_SOURCE：OpenWrt 的 musl 没有 __snprintf_chk 这类 glibc fortify 符号，
# 不关掉的话板上 dlopen 会报 "symbol not found"
CFLAGS  := -O2 -Wall -Wextra -fPIC -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
# musl（OpenWrt）没有独立的 libdl，dlopen 在 libc 里：板上用 make LDFLAGS=
LDFLAGS ?= -ldl

# 插件列表：新增插件只要往这里加一行
PLUGINS := math table batch doc net netcap video sysmon owrt sys music curl jsonq browser voice image

all: agent cgi mediad desub_fill desub_shot $(PLUGINS:%=plugins/%.so)

# luci.mk 的 Build/Compile 调的是 `make clean compile`
compile: all

test: all
	$(CC) $(CFLAGS) -I. -o tests/test_math tests/test_math.c
	$(CC) $(CFLAGS) -I. -o tests/test_json tests/test_json.c json.c
	$(CC) $(CFLAGS) -I. -o tests/test_plugin tests/test_plugin.c
	$(CC) $(CFLAGS) -I. -o tests/test_doc tests/test_doc.c json.c
	@echo "跑测试：./tests/test_math   ./tests/test_json   ./tests/test_doc <文件>"

# --wrap 把框架内的 malloc/free 接到 memcheck.c 的记账版本（--memcheck 模式用）
WRAP := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=strdup,--wrap=free

desub_fill: desub_fill.c
	$(CC) $(CFLAGS) -o $@ desub_fill.c $(LDFLAGS)

desub_shot: desub_shot.c
	$(CC) $(CFLAGS) -o $@ desub_shot.c $(LDFLAGS)

mediad: media.c
	$(CC) $(CFLAGS) -o $@ media.c $(LDFLAGS)

agent: agent.c json.c memcheck.c plugin.h json.h
	$(CC) $(CFLAGS) -o $@ agent.c json.c memcheck.c -lm $(WRAP) $(LDFLAGS)

# py_write.h 是 doc.c 内嵌的 python 脚本，改了它必须重编 doc.so
plugins/doc.so: py_write.h plugins/doc_read.inc
plugins/browser.so: py_browse.h
# curl_table.inc 是 curl.c include 进来的，改了它必须重编 curl.so
plugins/jsonq.so: plugins/json_table.inc
plugins/music.so: plugins/music_sheet.inc
plugins/%.so: plugins/%.c plugin.h
	$(CC) $(CFLAGS) -I. -shared -o $@ $< -lz -lm $(LDFLAGS)
	@mkdir -p plugins

# 安装清单是【白名单】：逐条 install 具体产物，不用 cp -r。
# 所以 src/tests/ 里的开发期测试脚本不会进 ipk（已核对：46 个文件，零 .py）。
# 以后加产物也请逐条写，别图省事改成 cp -r src/* —— 那会把测试脚本一起打进去。
install: all
	install -d $(DESTDIR)/opt/llama/agent/plugins
	install -d $(DESTDIR)/www/cgi-bin
	install -m 755 agent $(DESTDIR)/opt/llama/agent/agent-c
	install -m 755 cgi   $(DESTDIR)/www/cgi-bin/agent
	install -m 755 mediad $(DESTDIR)/opt/llama/agent/mediad
	install -m 755 desub_fill $(DESTDIR)/opt/llama/agent/desub_fill
	install -m 755 desub_shot $(DESTDIR)/opt/llama/agent/desub_shot
	install -m 755 $(PLUGINS:%=plugins/%.so) $(DESTDIR)/opt/llama/agent/plugins/
	install -m 755 memstat.sh $(DESTDIR)/opt/llama/agent/

clean:
	rm -f agent cgi mediad desub_fill desub_shot $(PLUGINS:%=plugins/%.so) tests/test_math tests/test_json tests/test_plugin tests/test_doc

.PHONY: all compile install clean test

cgi: cgi.c json.c json.h plugin.h
	$(CC) $(CFLAGS) -o $@ cgi.c json.c
