# flamingo-worker in a container: DXC's Linux release needs glibc 2.38 (Ubuntu 24.04), older hosts run it here.
# Build next to the files it copies (flamingo_worker.py, worker.txt and the dxc release unpacked in dxc/):
#   docker build -t flamingo-worker .
#   docker run -d --name flamingo-worker --restart unless-stopped --cpus 6 --memory 6g flamingo-worker
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /worker
COPY dxc/ dxc/
COPY flamingo_worker.py worker.txt ./
ENV LD_LIBRARY_PATH=/worker/dxc/lib
CMD ["python3", "-u", "flamingo_worker.py", "--jobs", "6"]
