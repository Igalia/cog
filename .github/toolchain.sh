#! /bin/bash
set -eu -o pipefail

# Info on available toolchains: https://wk-contrib.igalia.com/
INSTALL_DIR=${1:-${HOME}/toolchain}
BASEURL=https://people.igalia.com/psaavedra/toolchains
FILE=browsers-glibc-x86_64-core-image-weston-browsers-cortexa9t2hf-neon-wandboard-mesa-toolchain-1.0.sh
SUMS="$(pwd)/.github/toolchain.sha256"

declare -r INSTALL_DIR BASEURL FILE SUMS
declare -a curl_opts=( --http1.1 --retry 3 -L -C - )
declare -i tries=0
rm -f ~/toolchain.sh

function check_installer {
    local -i rc=1
    pushd ~ > /dev/null
    if sha256sum -c "${SUMS}" ; then
        rc=0
    fi
    popd > /dev/null
    return $rc
}

function fetch_installer {
    if [[ $(( tries++ )) -ge 10 ]] ; then
        echo 'Maximum amount of retries reached, bailing out.' 1>&2
        return 1
    fi

    local exit_code=0
    if curl "${curl_opts[@]}" -o ~/toolchain.sh "${BASEURL}/${FILE}" ; then
        if check_installer ; then
            return 0
        fi

        echo "Checksum mismatch, restarting download from scratch..." 1>&2
        rm -f ~/toolchain.sh
    else
        exit_code=$?

        if [[ ${exit_code} -eq 36 ]] ; then
            echo "Bad resume (${exit_code}), restarting download from scratch..." 1>&2
            rm -f ~/toolchain.sh
        else
            echo "Download error (${exit_code}), retrying download..." 1>&2
        fi
    fi

    local seconds=$(( RANDOM % 10 + 5 ))
    printf 'Waiting... %i' "${seconds}"
    while [[ $(( seconds-- )) -gt 0 ]] ; do
        sleep 1
        printf ' %i' "${seconds}"
    done
    echo '.'
    fetch_installer
}

if [[ -r ${INSTALL_DIR}/.installed ]] && \
    cmp -s "${INSTALL_DIR}/.installed" "${SUMS}"
then
    echo 'Cached toolchain already up to date'
    exit 0
fi

fetch_installer
echo 'Installing toolchain...'

rm -rf "${INSTALL_DIR}"
chmod +x ~/toolchain.sh
~/toolchain.sh -d "${INSTALL_DIR}" -y
sudo chown -R "${USER}" "${INSTALL_DIR}"
chmod -R u+r "${INSTALL_DIR}"
cp -v "${SUMS}" "${INSTALL_DIR}/.installed"
