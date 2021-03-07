#! /bin/bash
set -e

INSTALL_DIR=${1:-${HOME}/toolchain}
BASEURL=https://wk-contrib.igalia.com/yocto/meta-perf-browser/browsers/nightly/sdk
FILE=wandboard-mesa/browsers-glibc-x86_64-core-image-weston-browsers-armv7at2hf-neon-wandboard-mesa-toolchain-1.0.sh

declare -r INSTALL_DIR BASEURL FILE
rm -f ~/toolchain.sh

declare -a curl_opts=( --retry 3 -L )
if [[ -r ${INSTALL_DIR}/.installed ]] ; then
    curl_opts+=( --time-cond "${INSTALL_DIR}/.installed" )
fi

curl "${curl_opts[@]}" -o ~/toolchain.sh "${BASEURL}/${FILE}"

if [[ -r ~/toolchain.sh ]] ; then
    echo 'Installing toolchain...'

    rm -rf "${INSTALL_DIR}"
    chmod +x ~/toolchain.sh
    ~/toolchain.sh -d "${INSTALL_DIR}" -y
    sudo chown -R "${USER}" "${INSTALL_DIR}"
    chmod -R u+r "${INSTALL_DIR}"

    date -r ~/toolchain.sh -u -Iseconds > "${INSTALL_DIR}/.installed"
    touch -r ~/toolchain.sh "${INSTALL_DIR}/.installed"
else
    echo 'Cached toolchain already up to date'
fi
