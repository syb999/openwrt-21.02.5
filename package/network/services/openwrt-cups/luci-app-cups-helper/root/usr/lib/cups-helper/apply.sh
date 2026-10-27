#!/bin/sh
# cups-helper: apply tuning to cupsd.conf and publish queues over mDNS.
# Runs from LuCI (after saving settings) and from uci-defaults on first install.

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

# accept a comma/space separated list; keep only sane CIDR-looking entries
NETS=$(printf '%s\n' "$SUBNET" | tr ', \t' '\n\n\n' | while read -r n || [ -n "$n" ]; do
	case "$n" in
		""|*[!0-9./]*) continue ;;
	esac
	printf '%s\n' "$n"
done | sort -u)

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
	mv "$CUPSD_CONF.new" "$CUPSD_CONF"
	rm -f /tmp/.cups-helper-nets
fi

# ---------- 1. managed block inside cupsd.conf ----------
if [ -f "$CUPSD_CONF" ]; then
	awk '
		/^# BEGIN cups-helper/ { skip=1; next }
		/^# END cups-helper/   { skip=0; next }
		!skip                  { print }
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

	mv "$CUPSD_CONF.new" "$CUPSD_CONF"
fi

# ---------- 2. mDNS publication ----------
rm -f "$AVAHI_DIR"/cups-helper-*.service 2>/dev/null

if [ "$AVAHI" = "1" ] && [ -x /usr/sbin/avahi-daemon ] && [ -d "$AVAHI_DIR" ]; then
	# queue names come from printers.conf
	sed -n 's/^<Printer \(.*\)>$/\1/p' /etc/cups/printers.conf 2>/dev/null | while read -r q; do
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
    <txt-record>UUID=$(cat /etc/cups/cupsd.conf 2>/dev/null | md5sum | cut -c1-8)-$q</txt-record>
  </service>
</service-group>
EOF
	done
fi

# ---------- 3. restart what we touched ----------
/etc/init.d/cupsd restart >/dev/null 2>&1
[ -x /etc/init.d/avahi-daemon ] && /etc/init.d/avahi-daemon restart >/dev/null 2>&1

exit 0
