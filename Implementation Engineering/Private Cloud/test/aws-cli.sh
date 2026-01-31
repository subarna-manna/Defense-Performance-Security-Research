# Configure (fake credentials - no auth yet)
aws --endpoint-url http://localhost:9000 s3 ls s3://mybucket

# Upload
aws --endpoint-url http://localhost:9000 s3 cp file.txt s3://mybucket/path/to/file.txt

# Download
aws --endpoint-url http://localhost:9000 s3 cp s3://mybucket/path/to/file.txt downloaded.txt

# List
aws --endpoint-url http://localhost:9000 s3 ls s3://mybucket --recursive



# Initiate
aws --endpoint-url http://localhost:9000 s3api create-multipart-upload --bucket mybucket --key largefile.bin

# Upload parts (repeat for each chunk)
aws --endpoint-url http://localhost:9000 s3api upload-part --bucket mybucket --key largefile.bin --part-number 1 --upload-id <upload-id> --body part1.bin

# Complete (aws-cli generates correct XML automatically)
aws --endpoint-url http://localhost:9000 s3api complete-multipart-upload --bucket mybucket --key largefile.bin --upload-id <upload-id> --multipart-upload file://parts.json




