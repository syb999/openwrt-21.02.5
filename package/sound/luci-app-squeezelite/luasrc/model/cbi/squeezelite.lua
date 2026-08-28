local sys = require "luci.sys"

m = Map("squeezelite", translate("Squeezelite Audio Player"),
        translate("Squeezelite is a small headless squeezeplay emulator for Linux using ALSA audio output."))
m.apply_on_parse = true

m:section(SimpleSection).template  = "squeezelite_status"

s = m:section(TypedSection, "options", "")
s.anonymous = true
s.addremove = false

s:tab("general", translate("Setting"))

enabled = s:taboption("general", Flag, "enabled", translate("Enable"))
enabled.default = "1"
enabled.rmempty = false

name = s:taboption("general", Value, "name", translate("Player Name"))
name.default = "SqueezeWrt"
name.datatype = "string"

server_addr = s:taboption("general", Value, "server_addr", translate("LMS Server Address (optional)"))
server_addr.default = ""
server_addr.datatype = "string"
server_addr.optional = true
server_addr.description = translate("Leave empty for automatic discovery (default port 3483). Enter IP or IP:port to connect manually.")

server_port = s:taboption("general", Value, "server_port", translate("LMS Server Port"))
server_port.default = "3483"
server_port.datatype = "port"

device = s:taboption("general", Value, "device", translate("ALSA Device"))
device.default = "hw:0,0"
device:value("hw:0,0", "Hardware Device 0,0")
device:value("plughw:0,0", "Plug Device 0,0")
device:value("default", "Default Device")

volume = s:taboption("general", Value, "volume", translate("Volume (%)"))
volume.default = "80"
volume.datatype = "range(0,100)"
volume.description = translate("Headphone output volume applied at boot. 0-100 percent.")

extra_set = s:taboption("general", Flag, "extraset", translate("Extra settings"))
extra_set.default = "0"
extra_set.rmempty = false

model_name = s:taboption("general", Value, "model_name", translate("Model Name"))
model_name:depends("extraset", "1")
model_name.default = "SqueezeLite"
model_name.datatype = "string"

max_sr = s:taboption("general", Value, "max_sr", translate("Maximum Sample Rate (Hz)"))
max_sr:depends("extraset", "1")
max_sr.default = "48000"
max_sr:value("0", "Auto")
max_sr:value("44100", "44.1 kHz")
max_sr:value("48000", "48 kHz")
max_sr:value("88200", "88.2 kHz")
max_sr:value("96000", "96 kHz")
max_sr:value("192000", "192 kHz")
max_sr.datatype = "uinteger"

close_delay = s:taboption("general", Value, "close_delay", translate("Close Delay (ms)"))
close_delay:depends("extraset", "1")
close_delay.default = "0"
close_delay.datatype = "uinteger"

priority = s:taboption("general", Value, "priority", translate("Priority"))
priority:depends("extraset", "1")
priority.default = "0"
priority.datatype = "uinteger"

alsa_buffer = s:taboption("general", Value, "alsa_buffer", translate("ALSA Buffer (ms)"))
alsa_buffer:depends("extraset", "1")
alsa_buffer.default = "200"
alsa_buffer.datatype = "uinteger"

alsa_period = s:taboption("general", Value, "alsa_period", translate("ALSA Period (bytes)"))
alsa_period:depends("extraset", "1")
alsa_period.default = "20"
alsa_period.datatype = "uinteger"
alsa_period.description = translate("Period count if <50, period size in bytes if >=50. Recommended: 20 (period count). Large byte values cause XRUN busy-loop on stream interruption.")

dsd_over_pcm = s:taboption("general", ListValue, "dsd_over_pcm", translate("DSD over PCM"))
dsd_over_pcm:depends("extraset", "1")
dsd_over_pcm.default = "0"
dsd_over_pcm:value("0", "Disabled")
dsd_over_pcm:value("1", "Enabled")

ircontrol = s:taboption("general", ListValue, "ircontrol", translate("IR Control"))
ircontrol:depends("extraset", "1")
ircontrol.default = "0"
ircontrol:value("0", "Disabled")
ircontrol:value("1", "Enabled")

interface = s:taboption("general", Value, "interface", translate("Network Interface"))
interface:depends("extraset", "1")
interface.default = ""
interface.optional = true

codec_section = s:taboption("general", DummyValue, "codec_section", translate("Codec Support"))
codec_section:depends("extraset", "1")
codec_section.rawhtml = true
codec_section.value = ""

decode_mp3 = s:taboption("general", Flag, "decode_mp3", translate("MP3 Decode"))
decode_mp3:depends("extraset", "1")
decode_mp3.default = "1"
decode_mp3.rmempty = false

decode_flac = s:taboption("general", Flag, "decode_flac", translate("FLAC Decode"))
decode_flac:depends("extraset", "1")
decode_flac.default = "1"
decode_flac.rmempty = false

decode_aac = s:taboption("general", Flag, "decode_aac", translate("AAC Decode"))
decode_aac:depends("extraset", "1")
decode_aac.default = "1"
decode_aac.rmempty = false

decode_ogg = s:taboption("general", Flag, "decode_ogg", translate("OGG Decode"))
decode_ogg:depends("extraset", "1")
decode_ogg.default = "1"
decode_ogg.rmempty = false

decode_wma_alac = s:taboption("general", Flag, "decode_wma_alac", translate("WMA/ALAC Decode"))
decode_wma_alac:depends("extraset", "1")
decode_wma_alac.default = "1"
decode_wma_alac.rmempty = false

function m.on_after_apply(self)
    os.execute("echo \"APPLY $(date)\" > /tmp/apply_triggered")
    sys.call("/etc/init.d/squeezelite restart >/dev/null 2>&1")
    os.execute("v=\"$(uci get squeezelite.options.volume 2>/dev/null)\"; " ..
              "[ -n \"$v\" ] || v=80; amixer -c 0 sset 'Headphone' \"${v}%\" >/dev/null 2>&1")
end


return m

