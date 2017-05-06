#!/bin/sh
set -e
# Note: -u causes problems with posh, it barks at “undefined” $@ when no
# arguments provided.
test -z "$POSH_VERSION" && set -u

get_jobs_num() {
  local num="$(cat /proc/cpuinfo | grep -c "^processor")"
  num="$(echo $(( num + 1 )))"
  num="${num:-1}"
  echo $num
}

help() {
  echo 'Usage:'
  echo '  pvscheck.sh [--pvs URL] [--deps] [target-directory [branch]]'
  echo '  pvscheck.sh [--pvs URL] [--recheck] [target-directory]'
  echo '  pvscheck.sh [--pvs URL] --pvs-install {target-directory}'
  echo '  pvscheck.sh --patch [--only-build]'
  echo
  echo '    --pvs: Use the specified URL as a path to pvs-studio archive.'
  echo '           By default latest tested version is used.'
  echo
  echo '           May use `--pvs detect` to try detecting latest version.'
  echo '           That assumes certain viva64.com site properties and'
  echo '           may be broken by the site update.'
  echo
  echo '    --deps: (for regular run) Use top-level Makefile and build deps.'
  echo '            Without this it assumes all dependencies are already'
  echo '            installed.'
  echo
  echo '    --only-build: (for --patch) Only patch files in ./build directory.'
  echo
  echo '    --pvs-install: Only install PVS-studio to the specified location.'
  echo
  echo '    --patch: patch sources in the current directory.'
  echo '             Does not patch already patched files.'
  echo '             Does not run analysis.'
  echo
  echo '    --recheck: run analysis on a prepared target directory.'
  echo
  echo '    target-directory: Directory where build should occur.'
  echo '                      Default: ../neovim-pvs'
  echo
  echo '    branch: Branch to check.'
  echo '            Default: master.'
}

getopts_error() {
  printf '%s\n' "$1" >&2
  echo 'return 1'
  return 1
}

# Usage `eval "$(getopts_long long_defs -- positionals_defs -- "$@")"`
#
# long_defs: list of pairs of arguments like `longopt action`.
# positionals_defs: list of arguments like `action`.
#
# `action` is a space-separated commands:
#
#   store_const [const] [varname] [default]
#     Store constant [const] (default 1) (note: eval’ed) if argument is present
#     (long options only). Assumes long option accepts no arguments.
#   store [varname] [default]
#     Store value. Assumes long option needs an argument.
#   run {func} [varname] [default]
#     Run function {func} and store its output to the [varname]. Assumes no
#     arguments accepted (long options only).
#   modify {func} [varname] [default]
#     Like run, but assumes a single argument, passed to function {func} as $1.
#
#   All actions stores empty value if neither [varname] nor [default] are
#   present. [default] is evaled by top-level `eval`, so be careful. Also note
#   that no arguments may contain spaces, including [default] and [const].
getopts_long() {
  local positional=
  local opt_bases=""
  while test $# -gt 0 ; do
    local arg="$1" ; shift
    local opt_base=
    local act=
    local opt_name=
    if test -z "$positional" ; then
      if test "$arg" = "--" ; then
        positional=0
        continue
      fi
      act="$1" ; shift
      opt_name="$(echo "$arg" | tr '-' '_')"
      opt_base="longopt_$opt_name"
    else
      if test "$arg" = "--" ; then
        break
      fi
      : $(( positional+=1 ))
      act="$arg"
      opt_name="arg_$positional"
      opt_base="positional_$positional"
    fi
    opt_bases="$opt_bases $opt_base"
    eval "local varname_$opt_base=$opt_name"
    local i=0
    for act_subarg in $act ; do
      eval "local act_$(( i+=1 ))_$opt_base=\"\$act_subarg\""
    done
  done
  # Process options
  local positional=0
  local force_positional=
  while test $# -gt 0 ; do
    local argument="$1" ; shift
    local opt_base=
    local has_equal=
    local equal_arg=
    local is_positional=
    if test "$argument" = "--" ; then
      force_positional=1
      continue
    elif test -z "$force_positional" && test "${argument#--}" != "$argument"
    then
      local opt_name="${argument#--}"
      local opt_name_striparg="${opt_name%%=*}"
      if test "$opt_name" = "$opt_name_striparg" ; then
        has_equal=0
      else
        has_equal=1
        equal_arg="${argument#*=}"
        opt_name="$opt_name_striparg"
      fi
      # Use trailing x to prevent stripping newlines
      opt_name="$(printf '%sx' "$opt_name" | tr '-' '_')"
      opt_name="${opt_name%x}"
      if test -n "$(printf '%sx' "$opt_name" | tr -d 'a-z_')" ; then
        getopts_error "Option contains invalid characters: $opt_name"
      fi
      opt_base="longopt_$opt_name"
    else
      : $(( positional+=1 ))
      opt_base="positional_$positional"
      is_positional=1
    fi
    if test -n "$opt_base" ; then
      eval "local occurred_$opt_base=1"

      eval "local act_1=\"\$act_1_$opt_base\""
      eval "local varname=\"\$varname_$opt_base\""
      local need_val=
      local func=
      case "$act_1" in
        (store_const)
          eval "local const=\"\${act_2_${opt_base}:-1}\""
          eval "local varname=\"\${act_3_${opt_base}:-$varname}\""
          printf 'local %s=%s\n' "$varname" "$const"
          ;;
        (store)
          eval "varname=\"\${act_2_${opt_base}:-$varname}\""
          need_val=1
          ;;
        (run)
          eval "func=\"\${act_2_${opt_base}}\""
          eval "varname=\"\${act_3_${opt_base}:-$varname}\""
          printf 'local %s="$(%s)"\n' "$varname" "$func"
          ;;
        (modify)
          eval "func=\"\${act_2_${opt_base}}\""
          eval "varname=\"\${act_3_${opt_base}:-$varname}\""
          need_val=1
          ;;
      esac
      if test -n "$need_val" ; then
        local val=
        if test -z "$is_positional" ; then
          if test $has_equal = 1 ; then
            val="$equal_arg"
          else
            if test $# -eq 0 ; then
              getopts_error "Missing argument for $opt_name"
            fi
            val="$1" ; shift
          fi
        else
          val="$argument"
        fi
        local escaped_val="'$(printf "%s" "$val" | sed "s/'/'\\\\''/g")'"
        case "$act_1" in
          (store)
            printf 'local %s=%s\n' "$varname" "$escaped_val"
            ;;
          (modify)
            printf 'local %s="$(%s %s)"\n' "$varname" "$func" "$escaped_val"
            ;;
        esac
      fi
    fi
  done
  # Print default values when no values were provided
  local opt_base=
  for opt_base in $opt_bases ; do
    eval "local occurred=\"\${occurred_$opt_base:-}\""
    if test -n "$occurred" ; then
      continue
    fi
    eval "local act_1=\"\$act_1_$opt_base\""
    eval "local varname=\"\$varname_$opt_base\""
    case "$act_1" in
      (store)
        eval "local varname=\"\${act_2_${opt_base}:-$varname}\""
        eval "local default=\"\${act_3_${opt_base}:-}\""
        printf 'local %s=%s\n' "$varname" "$default"
        ;;
      (store_const|run|modify)
        eval "local varname=\"\${act_3_${opt_base}:-$varname}\""
        eval "local default=\"\${act_4_${opt_base}:-}\""
        printf 'local %s=%s\n' "$varname" "$default"
        ;;
    esac
  done
}

get_pvs_comment() {
  local tgt="$1" ; shift

  cat > "$tgt/pvs-comment" << EOF
// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

EOF
}

install_pvs() {(
  local tgt="$1" ; shift
  local pvs_url="$1" ; shift

  cd "$tgt"

  mkdir pvs-studio
  cd pvs-studio

  curl -L -o pvs-studio.tar.gz "$pvs_url"
  tar xzf pvs-studio.tar.gz
  rm pvs-studio.tar.gz
  local pvsdir="$(find . -maxdepth 1 -mindepth 1)"
  find "$pvsdir" -maxdepth 1 -mindepth 1 -exec mv '{}' . \;
  rmdir "$pvsdir"
)}

adjust_path() {
  if test -d "$tgt/pvs-studio" ; then
    local saved_pwd="$PWD"
    cd "$tgt/pvs-studio"
    export PATH="$PWD/bin${PATH+:}${PATH}"
    cd "$saved_pwd"
  fi
}

create_compile_commands() {(
  local tgt="$1" ; shift
  local deps="$1" ; shift

  export CC=clang
  export CFLAGS=' -O0 '

  if test -z "$deps" ; then
    mkdir -p "$tgt/build"
    (
      cd "$tgt/build"

      cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="$PWD/root"
      make -j"$(get_jobs_num)"
    )
  else
    (
      cd "$tgt"

      make -j"$(get_jobs_num)" CMAKE_EXTRA_FLAGS=" -DCMAKE_INSTALL_PREFIX=$PWD/root -DCMAKE_BUILD_TYPE=Debug "
    )
  fi
  find "$tgt/build/src/nvim/auto" -name '*.test-include.c' -delete
)}

# Warning: realdir below only cares about directories unlike realpath.
#
# realpath is not available in Ubuntu trusty yet.
realdir() {(
  local dir="$1"
  cd "$dir"
  printf '%s\n' "$PWD"
)}

patch_sources() {(
  local tgt="$1" ; shift
  local only_bulid="${1:-}"

  get_pvs_comment "$tgt"

  local sh_script='
    pvs_comment="$(cat pvs-comment ; echo -n EOS)"
    filehead="$(head -c $(( ${#pvs_comment} - 3 )) "$1" ; echo -n EOS)"
    if test "x$filehead" != "x$pvs_comment" ; then
      cat pvs-comment "$1" > "$1.tmp"
      mv "$1.tmp" "$1"
    fi
  '

  cd "$tgt"

  if test "$only_build" != "--only-build" ; then
    find \
      src/nvim test/functional/fixtures test/unit/fixtures \
      -name '*.c' \
      -exec /bin/sh -c "$sh_script" - '{}' \;
  fi

  find \
    build/src/nvim/auto build/config \
    -name '*.c' -not -name '*.test-include.c' \
    -exec /bin/sh -c "$sh_script" - '{}' \;

  rm pvs-comment
)}

run_analysis() {(
  local tgt="$1" ; shift

  cd "$tgt"

  pvs-studio-analyzer \
    analyze \
      --threads "$(get_jobs_num)" \
      --output-file PVS-studio.log \
      --verbose \
      --file build/compile_commands.json \
      --sourcetree-root .

  plog-converter -t xml -o PVS-studio.xml PVS-studio.log
  plog-converter -t errorfile -o PVS-studio.err PVS-studio.log
  plog-converter -t tasklist -o PVS-studio.tsk PVS-studio.log
)}

do_check() {
  local tgt="$1" ; shift
  local branch="$1" ; shift
  local pvs_url="$1" ; shift
  local deps="$1" ; shift

  git clone --branch="$branch" . "$tgt"

  install_pvs "$tgt" "$pvs_url"

  adjust_path "$tgt"

  create_compile_commands "$tgt" "$deps"

  run_analysis "$tgt"
}

do_recheck() {
  local tgt="$1"

  adjust_path "$tgt"

  create_compile_commands "$tgt" "$deps"

  run_analysis "$tgt"
}

detect_url() {
  local url="${1:-detect}"
  if test "$url" = detect ; then
    curl -L 'https://www.viva64.com/en/pvs-studio-download-linux/' \
    | grep -o 'https\{0,1\}://[^"<>]\{1,\}/pvs-studio[^/"<>]*\.tgz'
  else
    printf '%s' "$url"
  fi
}

main() {
  local def_pvs_url="http://files.viva64.com/pvs-studio-6.15.21741.1-x86_64.tgz"
  eval "$(
    getopts_long \
      help store_const \
      pvs 'modify detect_url pvs_url "${def_pvs_url}"' \
      patch store_const \
      only-build 'store_const --only-build' \
      recheck store_const \
      pvs-install store_const \
      deps store_const \
      -- \
      'modify realdir tgt "$PWD/../neovim-pvs"' \
      'store branch master' \
      -- "$@"
  )"

  if test -n "$help" ; then
    help
    return 0
  fi

  set -x

  if test -n "$patch" ; then
    patch_sources "$only_build" "$tgt"
  elif test -n "$pvs_install" ; then
    install_pvs "$tgt" "$pvs_url"
  elif test -n "$recheck" ; then
    do_recheck "$tgt"
  else
    do_check "$tgt" "$branch" "$pvs_url" "$deps"
  fi
}

main "$@"
