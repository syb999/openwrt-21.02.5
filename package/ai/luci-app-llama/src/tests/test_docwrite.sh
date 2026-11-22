#!/bin/sh
# 在板上跑：验证 doc 插件的写入能力（每种格式生成后立刻读回）
cd /tmp/cagent || exit 1
P=./test_plugin
D=plugins/doc.so
G=/tmp/llama_agent/gen
mkdir -p $G

run() {
  echo "$2" > /tmp/req.json
  printf '%-12s ' "$1"
  $P $D "$(cat /tmp/req.json)" 2>/dev/null | tail -1 | cut -c1-190
}

echo "=== 写入 ==="
run "text"    '{"op":"text","path":"/tmp/llama_agent/gen/a.txt","content":"第一行\n第二行\n第三行中文"}'
run "xlsx"    '{"op":"xlsx","path":"/tmp/llama_agent/gen/b.xlsx","sheets":[{"name":"销量","rows":[["月份","销量"],["1月",120],["2月",150]]}]}'
run "docx"    '{"op":"docx","path":"/tmp/llama_agent/gen/c.docx","blocks":[{"type":"h","text":"测试文档","level":1},{"type":"p","text":"这是正文段落。"},{"type":"li","text":"要点一"}]}'
run "pptx"    '{"op":"pptx","path":"/tmp/llama_agent/gen/d.pptx","slides":[{"title":"演示标题","subtitle":"副标题"},{"title":"第一页","bullets":["要点A","要点B"]}]}'
run "pdf"     '{"op":"pdf","path":"/tmp/llama_agent/gen/e.pdf","blocks":[{"type":"h","text":"PDF 标题"},{"type":"p","text":"这是 PDF 正文，包含中文测试。"}]}'

echo ""
echo "=== xlsx 改单元格 ==="
run "xlsx_edit" '{"op":"xlsx_edit","path":"/tmp/llama_agent/gen/b.xlsx","sheet":"销量","cell":"B3","value":999}'

echo ""
echo "=== 读回验证（同一个 doc 插件）==="
for f in a.txt b.xlsx c.docx d.pptx e.pdf; do
  printf '%-10s ' "$f"
  $P $D "{\"path\":\"$G/$f\"}" 2>/dev/null | tail -1 | cut -c1-170
done

echo ""
echo "=== 生成的文件 ==="
ls -l $G/ 2>/dev/null | awk '{print $5, $9}'
