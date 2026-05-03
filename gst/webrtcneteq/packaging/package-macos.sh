#!/usr/bin/env bash
set -euo pipefail

version="${1:?version/tag required}"
dist_dir="${2:?dist dir required}"
out_dir="${OUT_DIR:-out/webrtcneteq-release}"
package_name="${PACKAGE_NAME:-recallai-gstreamer-webrtcneteq}"

is_relocatable_dependency() {
  local dep="$1"

  case "${dep}" in
    /opt/homebrew/* | /usr/local/Cellar/* | /usr/local/opt/* | /usr/local/lib/*.dylib | */GStreamer.framework/*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

list_rpaths() {
  local dylib="$1"

  otool -l "${dylib}" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
    in_rpath && $1 == "path" { print $2; in_rpath = 0 }
  '
}

normalize_macos_load_paths() {
  local dylib="$1"
  local basename
  basename="$(basename "${dylib}")"

  install_name_tool -id "@rpath/${basename}" "${dylib}"

  while IFS= read -r rpath; do
    if is_relocatable_dependency "${rpath}"; then
      install_name_tool -delete_rpath "${rpath}" "${dylib}" || true
    fi
  done < <(list_rpaths "${dylib}")

  if ! list_rpaths "${dylib}" | grep -Fxq "@loader_path/.."; then
    install_name_tool -add_rpath "@loader_path/.." "${dylib}"
  fi

  while IFS= read -r dep; do
    case "${dep}" in
      "" | @* | ./* | /System/* | /usr/lib/*)
        continue
        ;;
    esac

    if is_relocatable_dependency "${dep}"; then
      install_name_tool -change "${dep}" "@rpath/$(basename "${dep}")" "${dylib}"
    fi
  done < <(otool -L "${dylib}" | tail -n +2 | sed -E 's/^[[:space:]]*([^[:space:]]+).*/\1/')

  if otool -L "${dylib}" | grep -E '^[[:space:]]*(/opt/homebrew|/usr/local/(Cellar|opt|lib)|.*/GStreamer\.framework/)'; then
    echo "packaged dylib still has non-relocatable dependency paths" >&2
    exit 1
  fi

  if list_rpaths "${dylib}" | grep -E '^(/opt/homebrew|/usr/local/(Cellar|opt|lib)|.*/GStreamer\.framework/)'; then
    echo "packaged dylib still has non-relocatable rpaths" >&2
    exit 1
  fi
}

tag_name="${version//\//-}"
arch="$(uname -m)"
plugin="${out_dir}/libgstwebrtcneteq.dylib"
if [[ ! -f "${plugin}" ]]; then
  echo "missing ${plugin}" >&2
  exit 1
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

install_dir="lib/gstreamer-1.0"
mkdir -p "${work_dir}/${install_dir}" "${dist_dir}"
cp "${plugin}" "${work_dir}/${install_dir}/libgstwebrtcneteq.dylib"
strip -x "${work_dir}/${install_dir}/libgstwebrtcneteq.dylib" || true
normalize_macos_load_paths "${work_dir}/${install_dir}/libgstwebrtcneteq.dylib"

raw_dylib="${dist_dir}/${package_name}-${tag_name}-macos-${arch}.dylib"
tar_path="${dist_dir}/${package_name}-${tag_name}-macos-${arch}.tar.gz"

cp "${work_dir}/${install_dir}/libgstwebrtcneteq.dylib" "${raw_dylib}"
tar -C "${work_dir}" -czf "${tar_path}" "${install_dir}/libgstwebrtcneteq.dylib"

(
  cd "${dist_dir}"
  shasum -a 256 \
    "$(basename "${raw_dylib}")" \
    "$(basename "${tar_path}")" \
    > "${package_name}-${tag_name}-macos-${arch}.sha256"
)
