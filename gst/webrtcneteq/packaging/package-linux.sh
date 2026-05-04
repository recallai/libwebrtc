#!/usr/bin/env bash
set -euo pipefail

version="${1:?version/tag required}"
dist_dir="${2:?dist dir required}"
out_dir="${OUT_DIR:-out/webrtcneteq-release}"
package_name="${PACKAGE_NAME:-recallai-gstreamer-webrtcneteq}"

tag_name="${version//\//-}"
deb_version="$(sed -E 's/^[^0-9]*//' <<< "${tag_name}")"
if [[ ! "${deb_version}" =~ ^[0-9][0-9A-Za-z.+:~%-]*$ ]]; then
  echo "tag ${version} does not contain a Debian-compatible version" >&2
  echo "use a tag like v1.2.3 or webrtcneteq-v1.2.3" >&2
  exit 1
fi

plugin="${out_dir}/libgstwebrtcneteq.so"
if [[ ! -f "${plugin}" ]]; then
  echo "missing ${plugin}" >&2
  exit 1
fi

deb_arch="${DEB_ARCH:-$(dpkg --print-architecture)}"
multiarch="${DEB_HOST_MULTIARCH:-$(dpkg-architecture -a "${deb_arch}" -qDEB_HOST_MULTIARCH)}"
strip_tool="${STRIP:-strip}"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

install_dir="usr/lib/${multiarch}/gstreamer-1.0"
mkdir -p "${work_dir}/pkg/${install_dir}" "${work_dir}/pkg/DEBIAN" "${dist_dir}"
cp "${plugin}" "${work_dir}/pkg/${install_dir}/libgstwebrtcneteq.so"
"${strip_tool}" --strip-unneeded "${work_dir}/pkg/${install_dir}/libgstwebrtcneteq.so" || true

raw_so="${dist_dir}/${package_name}-${tag_name}-linux-${deb_arch}.so"
cp "${work_dir}/pkg/${install_dir}/libgstwebrtcneteq.so" "${raw_so}"

installed_size_kb="$(du -sk "${work_dir}/pkg/usr" | awk '{print $1}')"
cat > "${work_dir}/pkg/DEBIAN/control" <<EOF
Package: ${package_name}
Version: ${deb_version}
Section: libs
Priority: optional
Architecture: ${deb_arch}
Maintainer: Recall.ai <engineering@recall.ai>
Depends: libc6 (>= 2.36), libgcc-s1 (>= 3.0), libglib2.0-0 (>= 2.74), libgstreamer1.0-0 (>= 1.22), libgstreamer-plugins-base1.0-0 (>= 1.22), libstdc++6 (>= 12)
Installed-Size: ${installed_size_kb}
Description: WebRTC NetEQ RTP Opus decoder GStreamer plugin
 A GStreamer plugin that decodes RTP/Opus audio through WebRTC NetEQ.
EOF

deb_path="${dist_dir}/${package_name}_${deb_version}_${deb_arch}.deb"
tar_path="${dist_dir}/${package_name}-${tag_name}-linux-${deb_arch}.tar.gz"

dpkg-deb --build --root-owner-group "${work_dir}/pkg" "${deb_path}"
tar -C "${work_dir}/pkg" -czf "${tar_path}" "${install_dir}/libgstwebrtcneteq.so"

(
  cd "${dist_dir}"
  sha256sum \
    "$(basename "${raw_so}")" \
    "$(basename "${deb_path}")" \
    "$(basename "${tar_path}")" \
    > "${package_name}-${tag_name}-linux-${deb_arch}.sha256"
)
