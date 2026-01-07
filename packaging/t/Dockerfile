FROM ubuntu:22.04

# Install headers and build tools
RUN apt-get update && apt-get install -y build-essential

# Copy the deb package
ARG DEB_FILE
WORKDIR /tmp
COPY ${DEB_FILE} ./come.deb

# Install the package
RUN dpkg -i come.deb

# Copy examples
COPY examples /app/examples

# Build and test examples
WORKDIR /app/examples
RUN come build hello.co
RUN come build string_demo.co
RUN come build come_demo.co

# Run the hello example by default
CMD ["./hello"]
