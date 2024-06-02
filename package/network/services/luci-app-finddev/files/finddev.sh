#!/bin/sh

function format_mac() {
    input_mac=$1
    if [ $(echo $input_mac | wc -c) -lt 14 ];then
        echo "$input_mac" | awk '{for(i=1;i<=length($0);i+=2) print substr($0,i,2)}' | sed ':a;N;$!ba;s/\n/:/g' | tr 'A-Z' 'a-z'
    else
        echo "$input_mac" | sed 's/-/:/g;s/ /:/g' | tr 'A-Z' 'a-z'
    fi
}

echo -n "" > /tmp/finddev_.tmp
target_dev=$(format_mac $(uci get finddev.@finddev[0].dev_mac))
interface_name=$(ifstatus $(uci get finddev.@finddev[0].search_if) | grep device | tail -n1 | cut -d ':' -f2 | cut -d '"' -f2)
ip_prefix=$(ifconfig "$interface_name" | grep "inet addr" | awk '{print$2}' | cut -d ':' -f2 | cut -d '.' -f-3)
output="$(cat /proc/net/arp | grep "$interface_name" | grep "$target_dev" | grep -E '[0-9]+' | awk '{print$1}')"

if [ $(echo $output | wc -c) -gt 2 ];then
    echo -n "$output" > /tmp/finddev_.tmp
else
    for i in $(seq 2 254);do
        ping ${ip_prefix}.$i -c1 -W1 >/dev/null 2>&1
        if [ $? -eq 1 ];then
            output="$(cat /proc/net/arp | grep "$interface_name" | grep "$target_dev" | grep -E '[0-9]+' | awk '{print$1}')"
            if [ $(echo $output | wc -c) -gt 2 ];then
                echo -n "$output" > /tmp/finddev_.tmp
                exit 0
            fi
        fi
    done
    echo -n "未找到设备!" > /tmp/finddev_.tmp
fi

