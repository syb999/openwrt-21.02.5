#!/bin/sh
# cups-helper: apply tuning to cupsd.conf and publish queues over mDNS.
# Runs from LuCI (after saving settings) and from uci-defaults on first install.

# "apply.sh mdns" = 只重建 mDNS 发布并重启 avahi（LuCI 增删改队列后调用，
# 免得改了队列还要手动点"应用优化设置"才被客户端搜到）
ONLY_MDNS=0
[ "$1" = "mdns" ] && ONLY_MDNS=1

CONF=cups_helper.main
CUPSD_CONF=/etc/cups/cupsd.conf
PPD_DIR=/etc/cups/ppd
AVAHI_DIR=/etc/avahi/services

opt() {
	v=$(uci -q get "$CONF.$1")
	[ -n "$v" ] || v="$2"
	echo "$v"
}

LOG_LEVEL=$(opt log_level warn)
MAX_JOBS=$(opt max_jobs 50)
MAX_JOBS_PP=$(opt max_jobs_per_printer 10)
KEEP_DAYS=$(opt keep_jobs_days 1)
LISTEN_ANY=$(opt listen_any 1)
AVAHI=$(opt avahi 1)
# no fallback here on purpose: absent or empty = leave the CUPS defaults untouched
SUBNET=$(uci -q get cups_helper.main.access_subnet 2>/dev/null)

# 只接受 a.b.c.d/p 且各段合法: 坏值写进 cupsd.conf 会让 cupsd 直接起不来
cidr_ok() {
	case "$1" in */*) ;; *) return 1 ;; esac
	o1=${1%%.*}; r=${1#*.}; o2=${r%%.*}; r=${r#*.}
	o3=${r%%.*}; r=${r#*.}; o4=${r%%/*}; p=${r#*/}
	for x in "$o1" "$o2" "$o3" "$o4" "$p"; do
		case "$x" in ""|*[!0-9]*) return 1 ;; esac
	done
	[ "$o1" -le 255 ] && [ "$o2" -le 255 ] && [ "$o3" -le 255 ] &&
		[ "$o4" -le 255 ] && [ "$p" -le 32 ]
}

# accept a comma/space separated list; keep only valid CIDRs
NETS=$(printf '%s\n' "$SUBNET" | tr ',[:space:]' '\n' | while read -r n || [ -n "$n" ]; do
	[ -n "$n" ] || continue
	cidr_ok "$n" && printf '%s\n' "$n"
done | sort -u)

if [ "$ONLY_MDNS" = "0" ]; then

# ---------- 0. restrict who may use CUPS (rewrite Allow lines) ----------
# CUPS ships <Location> blocks allowing 10/8, 172.16/12 and 192.168/16.
# If access_subnet is set, every "Allow from ..." inside a Location block is
# replaced by 127.0.0.1 + that subnet. Empty value leaves CUPS defaults alone.
if [ -n "$NETS" ] && [ -f "$CUPSD_CONF" ]; then
	printf '%s\n' "$NETS" > /tmp/.cups-helper-nets
	awk '
		BEGIN {
			n = 0
			while ((getline l < "/tmp/.cups-helper-nets") > 0)
				if (l != "") a[++n] = l
			close("/tmp/.cups-helper-nets")
		}
		/^[[:space:]]*<Location/     { inloc=1; done=0; print; next }
		/^[[:space:]]*<\/Location>/  { inloc=0; print; next }
		inloc && /^[[:space:]]*Allow[[:space:]]+from/ {
			if (!done) {
				print "  Allow from 127.0.0.1"
				for (i = 1; i <= n; i++)
					print "  Allow from " a[i]
				done = 1
			}
			next
		}
		{ print }
	' "$CUPSD_CONF" > "$CUPSD_CONF.new"
	# 只有 awk 成功才替换, 否则回滚 (cupsd.conf 被写坏 = CUPS 起不来)
	if [ -s "$CUPSD_CONF.new" ]; then
		cp -f "$CUPSD_CONF" "$CUPSD_CONF.cups-helper.bak"
		mv -f "$CUPSD_CONF.new" "$CUPSD_CONF"
	else
		rm -f "$CUPSD_CONF.new"
	fi
	rm -f /tmp/.cups-helper-nets
fi

# ---------- 1. managed block inside cupsd.conf ----------
if [ -f "$CUPSD_CONF" ]; then
	awk '
		/^# BEGIN cups-helper/ { skip=1; next }
		/^# END cups-helper/   { skip=0; next }
		# 顺手清掉旧版留下来的非法指令 (CUPS 2.3 不认识 MaxPrinterHistory)
		!skip && !/^[[:space:]]*MaxPrinterHistory([[:space:]]|$)/ { print }
	' "$CUPSD_CONF" > "$CUPSD_CONF.new"

	{
		echo "# BEGIN cups-helper"
		echo "LogLevel $LOG_LEVEL"
		echo "MaxJobs $MAX_JOBS"
		echo "MaxJobsPerPrinter $MAX_JOBS_PP"
		echo "MaxJobsPerUser 20"
		if [ "$KEEP_DAYS" = "0" ]; then
			echo "PreserveJobHistory No"
		else
			echo "PreserveJobHistory Yes"
		fi
		echo "PreserveJobFiles No"
		echo "MaxJobTime 0"
		if [ "$LISTEN_ANY" = "1" ]; then
			echo "Listen 0.0.0.0:631"
			echo "Listen [::]:631"
		else
			echo "Listen localhost:631"
		fi
		echo "# END cups-helper"
	} >> "$CUPSD_CONF.new"

	if [ -s "$CUPSD_CONF.new" ]; then
		[ -f "$CUPSD_CONF.cups-helper.bak" ] || cp -f "$CUPSD_CONF" "$CUPSD_CONF.cups-helper.bak"
		mv -f "$CUPSD_CONF.new" "$CUPSD_CONF"
	else
		rm -f "$CUPSD_CONF.new"
	fi
fi

fi

# ---------- 2. mDNS publication ----------
rm -f "$AVAHI_DIR"/cups-helper-*.service 2>/dev/null

if [ "$AVAHI" = "1" ] && [ -x /usr/sbin/avahi-daemon ] && [ -d "$AVAHI_DIR" ]; then
	# 队列名从 lpstat -v 取。不能只 sed printers.conf 的 <Printer X>：
	# 默认队列的标签是 <DefaultPrinter X>，会被漏掉 —— 而加上来的打印机默认就设成默认队列，
	# 结果就是"加好了但笔记本搜不到"
	lpstat -v 2>/dev/null | sed -n 's/^device for \(.*\): .*/\1/p' | while read -r q; do
		[ -n "$q" ] || continue
		case "$q" in
			*[!A-Za-z0-9_.-]*) continue ;;   # skip odd names
		esac

		ppd="$PPD_DIR/$q.ppd"
		if [ -f "$ppd" ]; then
			ty=$(sed -n 's/^\*NickName:[[:space:]]*"\(.*\)"$/\1/p' "$ppd" | head -1)
			pdl="application/pdf,application/postscript,image/jpeg,image/png,text/plain"
		else
			ty="Raw queue (client renders)"
			pdl=""
		fi
		[ -n "$ty" ] || ty="CUPS queue $q"

		cat > "$AVAHI_DIR/cups-helper-$q.service" <<EOF
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">%h $q</name>
  <service>
    <type>_ipp._tcp</type>
    <subtype>_universal._sub._ipp._tcp</subtype>
    <port>631</port>
    <txt-record>txtvers=1</txt-record>
    <txt-record>qtotal=1</txt-record>
    <txt-record>rp=printers/$q</txt-record>
    <txt-record>ty=$ty</txt-record>
    <txt-record>note=OpenWrt CUPS ($q)</txt-record>
    <txt-record>pdl=$pdl</txt-record>
    <txt-record>adminurl=http://%h:631/printers/$q</txt-record>
  </service>
</service-group>
EOF
	done
fi

# ---------- 3. restart what we touched ----------
if [ "$ONLY_MDNS" = "1" ]; then
	[ -x /etc/init.d/avahi-daemon ] && /etc/init.d/avahi-daemon restart >/dev/null 2>&1
	exit 0
fi
/etc/init.d/cupsd restart >/dev/null 2>&1
[ -x /etc/init.d/avahi-daemon ] && /etc/init.d/avahi-daemon restart >/dev/null 2>&1

exit 0
