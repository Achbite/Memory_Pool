# Makefile for Memory Pool Project

IMAGE_NAME := memory-pool-cpp17
CONTAINER_WORKDIR := /workspace
BUILD_DIR := build

.PHONY: image build test run shell clean

image:
	docker build -t $(IMAGE_NAME) .

build: image
	docker run --rm -v $(CURDIR):$(CONTAINER_WORKDIR) -w $(CONTAINER_WORKDIR) $(IMAGE_NAME) bash ./build.sh

test: build
	docker run --rm -v $(CURDIR):$(CONTAINER_WORKDIR) -w $(CONTAINER_WORKDIR) $(IMAGE_NAME) ctest --test-dir $(BUILD_DIR) --output-on-failure

run: build
	docker run --rm -it -v $(CURDIR):$(CONTAINER_WORKDIR) -w $(CONTAINER_WORKDIR) $(IMAGE_NAME) bash ./start.sh

shell: image
	docker run --rm -it -v $(CURDIR):$(CONTAINER_WORKDIR) -w $(CONTAINER_WORKDIR) $(IMAGE_NAME) bash -lc 'bash ./build.sh && exec bash'

clean:
	rm -rf $(BUILD_DIR)
