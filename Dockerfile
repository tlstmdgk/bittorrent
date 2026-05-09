
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ="America/New_York"


RUN apt-get -yq update && \
    apt-get -yq upgrade && \
    apt-get -yq install \
    git \
    tcpdump \
    nano \
    unzip \
    make \
    gcc \
    g++ \
    sudo \
    libssl-dev \
    libncurses-dev \
    libev-dev \
    cmake \
    protobuf-compiler \
    htop \
    curl \
    bison \
    flex \ 
    build-essential \
    valgrind && \
    ln -fs /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone
WORKDIR /app
COPY . /app

RUN make
