#! /bin/bash
set -eu -o pipefail

# Info on available toolchains: https://wk-contrib.igalia.com/
INSTALL_DIR=${1:-${HOME}/toolchain}
BASEURL=https://people.igalia.com/psaavedra/toolchains
FILE=browsers-glibc-x86_64-core-image-weston-browsers-cortexa9t2hf-neon-wandboard-mesa-toolchain-1.0.sh
SUMS="$(pwd)/.github/toolchain.sha256"

declare -r INSTALL_DIR BASEURL FILE SUMS
rm -f ~/toolchain.sh

declare -a curl_opts=( --http1.1 --retry 3 -L -C - )
if [[ -r ${INSTALL_DIR}/.installed ]] ; then
    curl_opts+=( --time-cond "${INSTALL_DIR}/.installed" )
fi

declare -i tries=0

function fetch_installer {
    if [[ $(( tries++ )) -ge 10 ]] ; then
        echo 'Maximum amount of retries reached, bailing out.' 1>&2
        return 1
    fi

    local exit_code=0
    if curl "${curl_opts[@]}" -o ~/toolchain.sh "${BASEURL}/${FILE}" ; then
        return 0
    else
        exit_code=$?

        if [[ ${exit_code} -eq 36 ]] ; then
            echo "Bad resume (${exit_code}), restarting download from scratch..." 1>&2
            rm -f ~/toolchain.sh
        else
            echo "Download error (${exit_code}), retrying download..." 1>&2
        fi
        local seconds=$(( RANDOM % 10 + 5 ))
        printf 'Waiting... %i' "${seconds}"
        while [[ $(( seconds-- )) -gt 0 ]] ; do
            sleep 1
            printf ' %i' "${seconds}"
        done
        echo '.'
        fetch_installer
    fi
}

fetch_installer
pushd ~ > /dev/null
sha256sum -c "${SUMS}"
popd > /dev/null

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
