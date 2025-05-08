# Base image: Ubuntu x86_64
FROM --platform=linux/amd64 ubuntu:18.04

ENV TZ=Asia/Seoul LANG=ko_KR.UTF-8 LANGUAGE=ko_KR.UTF-8

# Fix GPG error by switching to old-releases mirror
RUN sed -i 's|http://archive.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g' /etc/apt/sources.list && \
    sed -i 's|http://security.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g' /etc/apt/sources.list

# Install dependencies
RUN export DEBIAN_FRONTEND=noninteractive && \
    apt-get update && \
    apt-get install -y \
        locales tzdata \
        build-essential \
        gcc \
        gdb \
        vim \
        git \
        sudo \
        qemu-system-x86 \
        python3 && \
    locale-gen ko_KR.UTF-8 && \
    update-locale LANG=ko_KR.UTF-8

# Create user and add to sudoers
RUN useradd -m -s /bin/bash jungle && usermod -aG sudo jungle
RUN echo "jungle ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/jungle

USER jungle
WORKDIR /home/jungle

# Setup bashrc
RUN echo "source /workspaces/pintos_lab_docker/pintos-kaist/activate" >> /home/jungle/.bashrc
