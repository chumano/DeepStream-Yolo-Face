# YOLOv8-Face usage
Ubuntu 22.04 : `wsl -d Ubuntu`
**NOTE**: The yaml file is not required.

* [Convert model](#convert-model)
* [Compile the lib](#compile-the-lib)
* [Edit the config_infer_primary_yoloV8_face file](#edit-the-config_infer_primary_yolov8_face-file)

##

### Convert model

#### 1. Download the YOLOv8-Face repo and install the requirements
pip install torch --dry-run --report -
curl -s https://pypi.org/pypi/torch/json

```bash
git clone https://github.com/chumano/yolov8-face.git
cd yolov8-face
pip3 install -r requirements.txt
python3 setup.py install
pip3 install onnx onnxslim onnxruntime
 # supported OPS version 17
pip3 install onnx==1.12.0
pip3 install onnxruntime==1.13.1 onnxslim==0.1.82 onnxscript==0.5.7
pip3 install torch==2.0.1 torchvision==0.15.2
# For more details, visit https://onnxruntime.ai/docs/reference/compatibility.html

```

requirements.txt file:
```bash
matplotlib>=3.2.2 # for plotting results
numpy>=1.21.6,<2.0.0 # array computing
opencv-python>=4.6.0 # image/video processing
Pillow>=7.1.2 # image processing
PyYAML>=5.3.1 # config file parsing
requests>=2.23.0 # HTTP requests
scipy>=1.4.1 # scientific computing
torch>=1.7.0,<=2.0.1 # tensor computing
torchvision>=0.8.1,<0.16.0 # vision utils
```
 
```bash
pip freeze > requirements.txt

certifi==2026.1.4
charset-normalizer==3.4.4
cmake==4.2.1
colorama==0.4.6
coloredlogs==15.0.1
contourpy==1.3.2
cuda-bindings==12.9.4
cuda-pathfinder==1.3.3
cycler==0.12.1
filelock==3.20.3
flatbuffers==25.12.19
fonttools==4.61.1
fsspec==2026.1.0
humanfriendly==10.0
idna==3.11
Jinja2==3.1.6
kiwisolver==1.4.9
lit==18.1.8
markdown-it-py==4.0.0
MarkupSafe==3.0.3
matplotlib==3.10.8
mdurl==0.1.2
ml_dtypes==0.5.4
mpmath==1.3.0
networkx==3.4.2
numpy==1.26.4
nvidia-cublas-cu11==11.10.3.66
nvidia-cublas-cu12==12.1.3.1
nvidia-cuda-cupti-cu11==11.7.101
nvidia-cuda-cupti-cu12==12.1.105
nvidia-cuda-nvrtc-cu11==11.7.99
nvidia-cuda-nvrtc-cu12==12.1.105
nvidia-cuda-runtime-cu11==11.7.99
nvidia-cuda-runtime-cu12==12.1.105
nvidia-cudnn-cu11==8.5.0.96
nvidia-cudnn-cu12==8.9.2.26
nvidia-cufft-cu11==10.9.0.58
nvidia-cufft-cu12==11.0.2.54
nvidia-cufile-cu12==1.13.1.3
nvidia-curand-cu11==10.2.10.91
nvidia-curand-cu12==10.3.2.106
nvidia-cusolver-cu11==11.4.0.1
nvidia-cusolver-cu12==11.4.5.107
nvidia-cusparse-cu11==11.7.4.91
nvidia-cusparse-cu12==12.1.0.106
nvidia-cusparselt-cu12==0.7.1
nvidia-nccl-cu11==2.14.3
nvidia-nccl-cu12==2.18.1
nvidia-nvjitlink-cu12==12.8.93
nvidia-nvshmem-cu12==3.4.5
nvidia-nvtx-cu11==11.7.91
nvidia-nvtx-cu12==12.1.105
onnx==1.20.1
onnx-ir==0.1.15
onnxruntime==1.13.1
onnxscript==0.5.7
onnxslim==0.1.82
opencv-python==4.13.0.90
packaging==26.0
pandas==2.3.3
pillow==12.1.0
protobuf==6.33.4
psutil==7.2.2
Pygments==2.19.2
pyparsing==3.3.2
python-dateutil==2.9.0.post0
pytz==2025.2
PyYAML==6.0.3
requests==2.32.5
rich==14.3.1
scipy==1.15.3
seaborn==0.13.2
six==1.17.0
sympy==1.14.0
thop==0.1.1.post2209072238
torch==2.0.1
torchvision==0.15.2
tqdm==4.67.1
triton==2.0.0
typing_extensions==4.15.0
tzdata==2025.3
urllib3==2.6.3
```

**NOTE**: It is recommended to use Python virtualenv.

#### 2. Copy conversor

Copy the `export_yoloV8_face.py` file from `DeepStream-Yolo-Face/utils` directory to the `yolov8-face` folder.

#### 3. Download the model

Download the `pt` file from [YOLOv8-Face](https://github.com/derronqi/yolov8-face) repo.

**NOTE**: You can use your custom model.

#### 4. Convert model

Generate the ONNX model file (example for YOLOv8n-Face)

```bash
python3 export_yoloV8_face.py -w yolov8n-face.pt --dynamic
```
https://onnxruntime.ai/docs/reference/compatibility.html

**NOTE**: To change the inference size (defaut: 640)

```bash
-s SIZE
--size SIZE
-s HEIGHT WIDTH
--size HEIGHT WIDTH
```

Example for 1280

```
-s 1280
```

or

```
-s 1280 1280
```

**NOTE**: To simplify the ONNX model

```
--simplify
```

**NOTE**: To use dynamic batch-size (DeepStream >= 6.1)

```
--dynamic
```

**NOTE**: To use static batch-size (example for batch-size = 4)

```
--batch 4
```

#### 5. Copy generated files

Copy the generated ONNX model file and labels.txt file (if generated) to the `DeepStream-Yolo-Face` folder.

##

### Compile the lib

1. Open the `DeepStream-Yolo-Face` folder and compile the lib

2. Set the `CUDA_VER` according to your DeepStream version

```
export CUDA_VER=XY.Z
```

* x86 platform

  ```
  DeepStream 8.0 = 12.8
  DeepStream 7.1 = 12.6
  DeepStream 7.0 / 6.4 = 12.2
  DeepStream 6.3 = 12.1
  DeepStream 6.2 = 11.8
  DeepStream 6.1.1 = 11.7
  DeepStream 6.1 = 11.6
  DeepStream 6.0.1 / 6.0 = 11.4
  ```

* Jetson platform

  ```
  DeepStream 8.0 = 13.0
  DeepStream 7.1 = 12.6
  DeepStream 7.0 / 6.4 = 12.2
  DeepStream 6.3 / 6.2 / 6.1.1 / 6.1 = 11.4
  DeepStream 6.0.1 / 6.0 = 10.2
  ```

3. Make the lib

```
make -C nvdsinfer_custom_impl_Yolo_face clean && make -C nvdsinfer_custom_impl_Yolo_face
```

##

### Convert onnx to tensorrt engine (optional)
```bash
/usr/src/tensorrt/bin/trtexec \
   --onnx=/app/models/yolov8n-face.onnx \
   --saveEngine=/app/models/yolov8n-face.onnx_b1_gpu0_fp32.engine \
   --fp16 \
   --memPoolSize=workspace:2G \
   --minShapes=input:1x3x640x640 \
   --optShapes=input:1x3x640x640 \
   --maxShapes=input:4x3x640x640 \
   --shapes=input:1x3x640x640 \
   --device=0
```


### Edit the config_infer_primary_yoloV8_face file

Edit the `config_infer_primary_yoloV8_face.txt` file according to your model (example for YOLOv8n-Face)

```
[property]
...
onnx-file=yolov8n-face.onnx
...
num-detected-classes=1
...
parse-bbox-func-name=NvDsInferParseYoloFace
...
```

**NOTE**: The **DeepStream-Yolo-Face** requires

```
[property]
...
maintain-aspect-ratio=1
symmetric-padding=1
...
```
