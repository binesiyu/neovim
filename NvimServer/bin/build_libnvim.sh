#!/bin/bash
set -Eeuo pipefail

readonly clean=${clean:?"true or false"}
declare target; target="$(uname -m)"; readonly target

build_libnvim() {
  local -r deployment_target=$1

  # Brew's gettext does not get sym-linked to PATH
  export PATH="/opt/homebrew/opt/gettext/bin:/usr/local/opt/gettext/bin:${PATH}"

  make \
    SDKROOT="$(xcrun --show-sdk-path)" \
    MACOSX_DEPLOYMENT_TARGET="${deployment_target}" \
    CMAKE_EXTRA_FLAGS="-DGETTEXT_SOURCE=CUSTOM -DCMAKE_OSX_DEPLOYMENT_TARGET=${deployment_target}" \
    DEPS_CMAKE_FLAGS="-DCMAKE_OSX_DEPLOYMENT_TARGET=${deployment_target} -DCMAKE_CXX_COMPILER=$(xcrun -find c++)" \
    CMAKE_FLAGS="-DENABLE_LIBINTL=OFF -DCUSTOM_UI=1 -DFEAT_TUI=0 -DCMAKE_BUILD_TYPE=Release" \
    libnvim
}

main() {
  # This script is located in /NvimServer/bin and we have to go to /
  pushd "$(dirname "${BASH_SOURCE[0]}")/../.." >/dev/null

  echo "### Building libnvim"
  local deployment_target
  deployment_target=$(cat "./NvimServer/Resources/${target}_deployment_target.txt")
  readonly deployment_target

  if [[ "${clean}" == true ]]; then
    make distclean
  fi

  build_libnvim "${deployment_target}"

  popd >/dev/null
  echo "### Built libnvim"
}

main
