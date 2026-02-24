# Using Triton Inference Server with YOLO ONNX Model

Container Version: `nvcr.io/nvidia/tritonserver:24.12-py3`
Triton Inference Server Version: 2.53

https://docs.nvidia.com/deeplearning/triton-inference-server/archives/
triton-inference-server-2500/user-guide/docs/user_guide/performance_tuning.html

## Prepare the Model Repository

Place your ONNX model and the `config.pbtxt` file in the following structure:
```
model-repo/
  yolo/
    1/
      model.onnx
    config.pbtxt
```


## Check server run

```bash
curl -v localhost:8000/v2/health/ready

# POST /repository/index
curl -X POST localhost:8000/v2/repository/index
## Result:  [{"name":"traffic","version":"1","state":"READY"},{"name":"yoloface","version":"2","state":"READY"}]

```



## Sending Inference Requests
### Model Input/Output

- **Input:** `input` — shape `[None, 3, 640, 640]`, type `FP32`
- **Output:** `output` — shape `[None, 8400, 20]`, type `FP32`

### Example
Install the Triton client library if you haven't already:
```bash
wsl
source .venv/bin/activate

pip install tritonclient[http]==2.65.0
# or for gRPC
pip install tritonclient[grpc]==2.65.0
# or for all protocols
pip install tritonclient[all]==2.65.0
```

You can use the Triton Python client:
```python
import numpy as np
import tritonclient.http as httpclient

client = httpclient.InferenceServerClient(url="localhost:8000")
input_data = np.random.rand(1, 3, 640, 640).astype(np.float32)
inputs = [httpclient.InferInput("input", input_data.shape, "FP32")]
inputs[0].set_data_from_numpy(input_data)
outputs = [httpclient.InferRequestedOutput("output")]

response = client.infer("yolo", inputs=inputs, outputs=outputs)
result = response.as_numpy("output")
print(result.shape)

# infer with versioned model
response = client.infer("yolo", model_version="2", inputs=inputs, outputs=outputs)
```

## Run example with Triton client
```bash
wsl
source .venv/bin/activate

# Face detection
## test face detection with dummy input
python3 triton/test_face.py
## test face detection with real input
python3 triton/detect_face.py

# Traffic detection
## test traffic detection with dummy input
python3 triton/test_traffic.py
## test traffic detection with real input
python3 triton/detect_traffic.py

# Clear output annotations
rm  images/*_annotated.jpg
```


## Triton Inference Server
```bash
tritonserver --help

Usage: tritonserver [options]
  --help
        Print usage

Server:
  --id <string>
        Identifier for this server.
  --exit-timeout-secs <integer>
        Timeout (in seconds) when exiting to wait for in-flight
        inferences to finish. After the timeout expires the server exits even if
        inferences are still in flight.

Logging:
  --log-verbose <integer>
        Set verbose logging level. Zero (0) disables verbose logging
        and values >= 1 enable verbose logging.
  --log-info <boolean>
        Enable/disable info-level logging.
  --log-warning <boolean>
        Enable/disable warning-level logging.
  --log-error <boolean>
        Enable/disable error-level logging.
  --log-format <string>
        Set the logging format. Options are "default" and "ISO8601".
        The default is "default". For "default", the log severity (L) and
        timestamp will be logged as "LMMDD hh:mm:ss.ssssss". For "ISO8601",
        the log format will be "YYYY-MM-DDThh:mm:ssZ L".
  --log-file <string>
        Set the name of the log output file. If specified, log
        outputs will be saved to this file. If not specified, log outputs will
        stream to the console.

Model Repository:
  --model-store <string>
        Equivalent to --model-repository.
  --model-repository <string>
        Path to model repository directory. It may be specified
        multiple times to add multiple model repositories. Note that if a model
        is not unique across all model repositories at any time, the model
        will not be available.
  --exit-on-error <boolean>
        Exit the inference server if an error occurs during
        initialization.
  --disable-auto-complete-config
        If set, disables the triton and backends from auto
        completing model configuration files. Model configuration files must be
        provided and all required configuration settings must be specified.
  --strict-readiness <boolean>
        If true /v2/health/ready endpoint indicates ready if the
        server is responsive and all models are available. If false
        /v2/health/ready endpoint indicates ready if server is responsive even if
        some/all models are unavailable.
  --model-control-mode <string>
        Specify the mode for model management. Options are "none",
        "poll" and "explicit". The default is "none". For "none", the server
        will load all models in the model repository(s) at startup and will
        not make any changes to the load models after that. For "poll", the
        server will poll the model repository(s) to detect changes and will
        load/unload models based on those changes. The poll rate is
        controlled by 'repository-poll-secs'. For "explicit", model load and unload
        is initiated by using the model control APIs, and only models
        specified with --load-model will be loaded at startup.
  --repository-poll-secs <integer>
        Interval in seconds between each poll of the model
        repository to check for changes. Valid only when --model-control-mode=poll is
        specified.
  --load-model <string>
        Name of the model to be loaded on server startup. It may be
        specified multiple times to add multiple models. To load ALL models
        at startup, specify '*' as the model name with --load-model=* as the
        ONLY --load-model argument, this does not imply any pattern
        matching. Specifying --load-model=* in conjunction with another
        --load-model argument will result in error. Note that this option will only
        take effect if --model-control-mode=explicit is true.
  --model-config-name <string>
        The custom configuration name for models to load.The name
        should not contain any space character.For example:
        --model-config-name=h100. If --model-config-name is not set, Triton will use the
        default config.pbtxt.
  --model-load-thread-count <integer>
        The number of threads used to concurrently load models in
        model repositories. Default is 4.
  --model-load-retry-count <integer>
        The number of retry to load a model in model repositories.
        Default is 0.
  --model-namespacing <boolean>
        Whether model namespacing is enable or not. If true, models
        with the same name can be served if they are in different namespace.
  --enable-peer-access <boolean>
        Whether the server tries to enable peer access or not. Even
        when this options is set to true,  peer access could still be not
        enabled because the underlying system doesn't support it. The server
        will log a warning in this case. Default is true.

HTTP:
  --allow-http <boolean>
        Allow the server to listen for HTTP requests.
  --http-address <string>
        The address for the http server to bind to. Default is
        0.0.0.0
  --http-port <integer>
        The port for the server to listen on for HTTP requests.
        Default is 8000.
  --reuse-http-port <boolean>
        Allow multiple servers to listen on the same HTTP port when
        every server has this option set. If you plan to use this option as
        a way to load balance between different Triton servers, the same
        model repository or set of models must be used for every server.
  --http-header-forward-pattern <string>
        The regular expression pattern that will be used for
        forwarding HTTP headers as inference request parameters.
  --http-thread-count <integer>
        Number of threads handling HTTP requests.
  --http-restricted-api <<string>:<string>=<string>>
        Specify restricted HTTP api setting. The format of this flag
        is --http-restricted-api=<apis>,<key>=<value>. Where <api> is a
        comma-separated list of apis to be restricted. <key> will be additional
        header key to be checked when a HTTP request is received, and
        <value> is the value expected to be matched. Allowed APIs: health,
        metadata, inference, shared-memory, model-config, model-repository,
        statistics, trace, logging

GRPC:
  --allow-grpc <boolean>
        Allow the server to listen for GRPC requests.
  --grpc-address <string>
        The address for the grpc server to binds to. Default is
        0.0.0.0
  --grpc-port <integer>
        The port for the server to listen on for GRPC requests.
        Default is 8001.
  --reuse-grpc-port <boolean>
        Allow multiple servers to listen on the same GRPC port when
        every server has this option set. If you plan to use this option as
        a way to load balance between different Triton servers, the same
        model repository or set of models must be used for every server.
  --grpc-header-forward-pattern <string>
        The regular expression pattern that will be used for
        forwarding GRPC headers as inference request parameters.
  --grpc-infer-allocation-pool-size <integer>
        The maximum number of inference request/response objects
        that remain allocated for reuse. As long as the number of in-flight
        requests doesn't exceed this value there will be no
        allocation/deallocation of request/response objects.
  --grpc-use-ssl <boolean>
        Use SSL authentication for GRPC requests. Default is false.
  --grpc-use-ssl-mutual <boolean>
        Use mututal SSL authentication for GRPC requests. This
        option will preempt '--grpc-use-ssl' if it is also specified. Default is
        false.
  --grpc-server-cert <string>
        File holding PEM-encoded server certificate. Ignored unless
        --grpc-use-ssl is true.
  --grpc-server-key <string>
        File holding PEM-encoded server key. Ignored unless
        --grpc-use-ssl is true.
  --grpc-root-cert <string>
        File holding PEM-encoded root certificate. Ignore unless
        --grpc-use-ssl is false.
  --grpc-infer-response-compression-level <string>
        The compression level to be used while returning the infer
        response to the peer. Allowed values are none, low, medium and high.
        By default, compression level is selected as none.
  --grpc-keepalive-time <integer>
        The period (in milliseconds) after which a keepalive ping is
        sent on the transport. Default is 7200000 (2 hours).
  --grpc-keepalive-timeout <integer>
        The period (in milliseconds) the sender of the keepalive
        ping waits for an acknowledgement. If it does not receive an
        acknowledgment within this time, it will close the connection. Default is
        20000 (20 seconds).
  --grpc-keepalive-permit-without-calls <boolean>
        Allows keepalive pings to be sent even if there are no calls
        in flight (0 : false; 1 : true). Default is 0 (false).
  --grpc-http2-max-pings-without-data <integer>
        The maximum number of pings that can be sent when there is
        no data/header frame to be sent. gRPC Core will not continue sending
        pings if we run over the limit. Setting it to 0 allows sending pings
        without such a restriction. Default is 2.
  --grpc-http2-min-recv-ping-interval-without-data <integer>
        If there are no data/header frames being sent on the
        transport, this channel argument on the server side controls the minimum
        time (in milliseconds) that gRPC Core would expect between receiving
        successive pings. If the time between successive pings is less than
        this time, then the ping will be considered a bad ping from the peer.
        Such a ping counts as a ‘ping strike’. Default is 300000 (5
        minutes).
  --grpc-http2-max-ping-strikes <integer>
        Maximum number of bad pings that the server will tolerate
        before sending an HTTP2 GOAWAY frame and closing the transport.
        Setting it to 0 allows the server to accept any number of bad pings.
        Default is 2.
  --grpc-max-connection-age <integer>
        Maximum time that a channel may exist in
        milliseconds.Default is undefined.
  --grpc-max-connection-age-grace <integer>
        Grace period after the channel reaches its max age. Default
        is undefined.
  --grpc-restricted-protocol <<string>:<string>=<string>>
        Specify restricted GRPC protocol setting. The format of this
        flag is --grpc-restricted-protocol=<protocols>,<key>=<value>. Where
        <protocol> is a comma-separated list of protocols to be restricted.
        <key> will be additional header key to be checked when a GRPC
        request is received, and <value> is the value expected to be matched.
        Allowed protocols: health, metadata, inference, shared-memory,
        model-config, model-repository, statistics, trace, logging

Sagemaker:
  --allow-sagemaker <boolean>
        Allow the server to listen for Sagemaker requests. Default
        is false.
  --sagemaker-port <integer>
        The port for the server to listen on for Sagemaker requests.
        Default is 8080.
  --sagemaker-safe-port-range <<integer>-<integer>>
        Set the allowed port range for endpoints other than the
        SageMaker endpoints.
  --sagemaker-thread-count <integer>
        Number of threads handling Sagemaker requests. Default is 8.

Vertex:
  --allow-vertex-ai <boolean>
        Allow the server to listen for Vertex AI requests. Default
        is true if AIP_MODE=PREDICTION, false otherwise.
  --vertex-ai-port <integer>
        The port for the server to listen on for Vertex AI requests.
        Default is AIP_HTTP_PORT if set, 8080 otherwise.
  --vertex-ai-thread-count <integer>
        Number of threads handling Vertex AI requests. Default is 8.
  --vertex-ai-default-model <string>
        The name of the model to use for single-model inference
        requests.

Metrics:
  --allow-metrics <boolean>
        Allow the server to provide prometheus metrics.
  --allow-gpu-metrics <boolean>
        Allow the server to provide GPU metrics. Ignored unless
        --allow-metrics is true.
  --allow-cpu-metrics <boolean>
        Allow the server to provide CPU metrics. Ignored unless
        --allow-metrics is true.
  --metrics-address <string>
        The address for the metrics server to bind to. Default is
        the same as --http-address if built with HTTP support. Otherwise,
        default is 0.0.0.0
  --metrics-port <integer>
        The port reporting prometheus metrics. Default is 8002.
  --metrics-interval-ms <float>
        Metrics will be collected once every <metrics-interval-ms>
        milliseconds. Default is 2000 milliseconds.
  --metrics-config <<string>=<string>>
        Specify a metrics-specific configuration setting. The format
        of this flag is --metrics-config=<setting>=<value>. It can be
        specified multiple times.

Tracing:
  --trace-config <<string>,<string>=<string>>
        Specify global or trace mode specific configuration setting.
        The format of this flag is --trace-config <mode>,<setting>=<value>.
        Where <mode> is either "triton" or "opentelemetry". The default is
        "triton". To specify global trace settings (level, rate, count, or
        mode), the format would be --trace-config <setting>=<value>. For
        "triton" mode, the server will use Triton's Trace APIs. For
        "opentelemetry" mode, the server will use OpenTelemetry's APIs to generate,
        collect and export traces for individual inference requests.

Backend:
  --backend-directory <string>
        The global directory searched for backend shared libraries.
        Default is '/opt/tritonserver/backends'.
  --backend-config <<string>,<string>=<string>>
        Specify a backend-specific configuration setting. The format
        of this flag is --backend-config=<backend_name>,<setting>=<value>.
        Where <backend_name> is the name of the backend, such as 'tensorrt'.

Repository Agent:
  --repoagent-directory <string>
        The global directory searched for repository agent shared
        libraries. Default is '/opt/tritonserver/repoagents'.

Response Cache:
  --cache-config <<string>,<string>=<string>>
        Specify a cache-specific configuration setting. The format
        of this flag is --cache-config=<cache_name>,<setting>=<value>. Where
        <cache_name> is the name of the cache, such as 'local' or 'redis'.
        Example: --cache-config=local,size=1048576 will configure a 'local'
        cache implementation with a fixed buffer pool of size 1048576 bytes.
  --cache-directory <string>
        The global directory searched for cache shared libraries.
        Default is '/opt/tritonserver/caches'. This directory is expected to
        contain a cache implementation as a shared library with the name
        'libtritoncache.so'.

Rate Limiter:
  --rate-limit <string>
        Specify the mode for rate limiting. Options are
        "execution_count" and "off". The default is "off". For "execution_count", the
        server will determine the instance using configured priority and the
        number of time the instance has been used to run inference. The
        inference will finally be executed once the required resources are
        available. For "off", the server will ignore any rate limiter config and
        run inference as soon as an instance is ready.
  --rate-limit-resource <<string>:<integer>:<integer>>
        The number of resources available to the server. The format
        of this flag is
        --rate-limit-resource=<resource_name>:<count>:<device>. The <device> is optional and if not listed will be applied to
        every device. If the resource is specified as "GLOBAL" in the model
        configuration the resource is considered shared among all the devices
        in the system. The <device> property is ignored for such resources.
        This flag can be specified multiple times to specify each resources
        and their availability. By default, the max across all instances
        that list the resource is selected as its availability. The values for
        this flag is case-insensitive.

Memory/Device Management:
  --pinned-memory-pool-byte-size <integer>
        The total byte size that can be allocated as pinned system
        memory. If GPU support is enabled, the server will allocate pinned
        system memory to accelerate data transfer between host and devices
        until it exceeds the specified byte size. If 'numa-node' is configured
        via --host-policy, the pinned system memory of the pool size will be
        allocated on each numa node. This option will not affect the
        allocation conducted by the backend frameworks. Default is 256 MB.
  --cuda-memory-pool-byte-size <<integer>:<integer>>
        The total byte size that can be allocated as CUDA memory for
        the GPU device. If GPU support is enabled, the server will allocate
        CUDA memory to minimize data transfer between host and devices
        until it exceeds the specified byte size. This option will not affect
        the allocation conducted by the backend frameworks. The argument
        should be 2 integers separated by colons in the format <GPU device
        ID>:<pool byte size>. This option can be used multiple times, but only
        once per GPU device. Subsequent uses will overwrite previous uses for
        the same GPU device. Default is 64 MB.
  --cuda-virtual-address-size <<integer>:<integer>>
        The total CUDA virtual address size that will be used for
        each implicit state when growable memory is used. This value
        determines the maximum size of each implicit state. The state size cannot go
        beyond this value. The argument should be 2 integers separated by
        colons in the format <GPU device ID>:<CUDA virtual address size>. This
        option can be used multiple times, but only once per GPU device.
        Subsequent uses will overwrite previous uses for the same GPU device.
        Default is 1 GB.
  --min-supported-compute-capability <float>
        The minimum supported CUDA compute capability. GPUs that
        don't support this compute capability will not be used by the server.
  --buffer-manager-thread-count <integer>
        The number of threads used to accelerate copies and other
        operations required to manage input and output tensor contents.
        Default is 0.
  --host-policy <<string>,<string>=<string>>
        Specify a host policy setting associated with a policy name.
        The format of this flag is
        --host-policy=<policy_name>,<setting>=<value>. Currently supported settings are 'numa-node', 'cpu-cores'.
        Note that 'numa-node' setting will affect pinned memory pool behavior,
        see --pinned-memory-pool for more detail.
  --model-load-gpu-limit <<device_id>:<fraction>>
        Specify the limit on GPU memory usage as a fraction. If
        model loading on the device is requested and the current memory usage
        exceeds the limit, the load will be rejected. If not specified, the
        limit will not be set.

DEPRECATED:
  --strict-model-config <boolean>
        DEPRECATED: If true model configuration files must be
        provided and all required configuration settings must be specified. If
        false the model configuration may be absent or only partially specified
        and the server will attempt to derive the missing required
        configuration.
  --response-cache-byte-size <integer>
        DEPRECATED: Please use --cache-config instead.
  --trace-file <string>
        DEPRECATED: Please use --trace-config
        triton,file=<path/to/your/file> Set the file where trace output will be saved. If
        --trace-log-frequency is also specified, this argument value will be the
        prefix of the files to save the trace output. See --trace-log-frequency
        for detail.
  --trace-level <string>
        DEPRECATED: Please use --trace-config
        level=<OFF|TIMESTAMPS|TENSORS>Specify a trace level. OFF to disable tracing, TIMESTAMPS to
        trace timestamps, TENSORS to trace tensors. It may be specified
        multiple times to trace multiple information. Default is OFF.
  --trace-rate <integer>
        DEPRECATED: Please use --trace-config rate=<rate value>Set
        the trace sampling rate. Default is 1000.
  --trace-count <integer>
        DEPRECATED: Please use --trace-config count=<count value>Set
        the number of traces to be sampled. If the value is -1, the number
        of traces to be sampled will not be limited. Default is -1.
  --trace-log-frequency <integer>
        DEPRECATED: Please use --trace-config
        triton,log-frequency=<value>Set the trace log frequency. If the value is 0, Triton will
        only log the trace output to <trace-file> when shutting down.
        Otherwise, Triton will log the trace output to <trace-file>.<idx> when it
        collects the specified number of traces. For example, if the log
        frequency is 100, when Triton collects the 100-th trace, it logs the
        traces to file <trace-file>.0, and when it collects the 200-th trace, it
        logs the 101-th to the 200-th traces to file <trace-file>.1.
        Default is 0.
```

## References
- [Release Notes for Triton Inference Server](https://docs.nvidia.com/deeplearning/triton-inference-server/release-notes/rel-25-05.html)
- [Triton Inference Server Old version](https://docs.nvidia.com/deeplearning/triton-inference-server/archives/triton-inference-server-2500/user-guide/docs/user_guide/performance_tuning.html)
- [Triton Server Container](https://catalog.ngc.nvidia.com/orgs/nvidia/containers/tritonserver/tags?version=25.12-py3)
- [Triton Inference Server Documentation](https://github.com/triton-inference-server/server)
- [Triton Inference Server Model Management](https://github.com/triton-inference-server/server/blob/main/docs/user_guide/model_management.md)
- [Triton Inference Server Model Configuration](https://github.com/triton-inference-server/server/blob/main/docs/user_guide/model_configuration.md)
- [Triton Inference Server Tutorials](https://github.com/triton-inference-server/tutorials)
- [Triton Python Client](https://github.com/triton-inference-server/client)
