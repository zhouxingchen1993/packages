#!/bin/bash

if [ -z "${2}" ] ; then
    echo "Usage: <solr server ip> <data file or data directory> [ <solr server port> ] "
    exit 0
fi

host="${1}"
port=${3:-8983}
data="${2}"

if [ -d "${data}" ] ; then
    data="${data}/*"
fi

core="e-commerce"
post_cmd="post"

if [ -z "$(which post 2>/dev/null)" ] ; then
    post_cmd="opt/solr/bin/post"
fi

${post_cmd} -p "${port}" -host "${host}" -c "${core}" ${1} -params "fieldnames=productid,cateid,title,description,manufacturer,price,quatity,size,url&header=true" -params "separator=%09"
