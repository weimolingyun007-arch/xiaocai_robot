.PHONY: build ota deploy

build:
	bash bringup/ota/build.sh

ota:
	bash bringup/ota/package.sh

deploy:
	bash bringup/ota/deploy_adb.sh
