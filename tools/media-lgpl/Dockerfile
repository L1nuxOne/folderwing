FROM ubuntu@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates build-essential gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev cmake ninja-build meson nasm pkg-config python3 python3-jinja2 python3-mako git curl xz-utils zip bzip2 patch && mkdir -p /toolchain-record && dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' > /toolchain-record/packages.tsv && cp -a /var/lib/apt/lists /toolchain-record/apt-lists
WORKDIR /work
