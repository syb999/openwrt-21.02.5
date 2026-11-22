import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
drv.set_window_size(1400, 950)
try:
    drv.get("http://127.0.0.1/llama.htm?g=1")
    time.sleep(3)
    st = drv.execute_script(
        "var b=document.getElementById('setbtn');"
        "if(!b) return {exists:false};"
        "var r=b.getBoundingClientRect();"
        "return {exists:true, w:Math.round(r.width), h:Math.round(r.height),"
        "        visible:(r.width>0&&r.height>0), title:b.title};")
    print("齿轮按钮:", st)
    if st.get("exists"):
        drv.execute_script("document.getElementById('setbtn').click();")
        time.sleep(1.5)
        r = drv.execute_script(
            "var m=document.querySelector('.qask-mask');"
            "return {opened:!!m, plugs: !!(m&&m.querySelector('#pluglist')),"
            "        n: (m?m.querySelectorAll('.plugrow').length:0)};")
        print("点击后:", r)
        drv.save_screenshot("/tmp/llama_agent/shots/gear.png")
finally:
    drv.quit()
