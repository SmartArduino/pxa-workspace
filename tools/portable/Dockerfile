FROM docker.io/library/ubuntu@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS prepared
COPY devkit /opt/pxa
RUN chmod -R a+rX /opt/pxa

FROM docker.io/library/ubuntu@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
COPY --from=prepared /opt/pxa /opt/pxa
ENV PATH="/opt/pxa/bin:${PATH}" \
    XDG_DATA_HOME=/pxa-data \
    XDG_CACHE_HOME=/pxa-cache \
    SDL_VIDEODRIVER=dummy \
    SDL_AUDIODRIVER=dummy
WORKDIR /workspace
ENTRYPOINT ["/opt/pxa/bin/pxa"]
