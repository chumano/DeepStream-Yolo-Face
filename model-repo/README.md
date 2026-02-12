# Model repository for Triton Inference Server

https://docs.nvidia.com/deeplearning/triton-inference-server/user-guide/docs/user_guide/model_repository.html

## Model Structure
```
  <model-repository-path>/
    <model-name>/
      config.pbtxt
      1/
        model.onnx/
           model.onnx
           <other model files>
```

## Tensor plan
```bash
docker exec -it ds-triton bash
/usr/src/tensorrt/bin/trtexec --onnx=/models/traffic/1/traffic.onnx  --saveEngine=/models/traffic/1/model.plan
```

