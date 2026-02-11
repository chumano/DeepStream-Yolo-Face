# Kafka Consumer

## Create Virtual Environment
```bash
wsl
python3 -m venv .venv
``

## Run
```bash
# run in wsl
wsl
# activate virtual environment
source .venv/bin/activate
# run consumer
cd ./consumers
python3 kafka_consumer.py
```


### Using Python Consumer

Create a file `kafka_consumer.py`:
`pip3 install kafka-python`

Run the consumer:
```bash
python3 kafka_consumer.py


SAVE_LANDMARKS=True python3 kafka_consumer.py
```