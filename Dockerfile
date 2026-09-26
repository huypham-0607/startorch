# ----------------------------------------------------------------------------
# Build Setup
# ----------------------------------------------------------------------------

FROM python:3.14-slim AS build

# Installing required packages
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates
RUN pip install --no-cache-dir uv==0.12.19

WORKDIR /app

# Copy dependencies list for python
COPY python/pyproject.toml python/uv.lock python/
RUN cd python && uv sync --frozen --no-dev --no-install-project

# Copy cpp source code + build
COPY cpp/ cpp/
RUN cmake -S cpp -B cpp/build && cmake --build cpp/build -j"$(nproc)" --target startorch_cpp

# Copy python source
COPY python/ python/
COPY project-config.toml ./

# Install the startorch package itself into the venv. The sync above installed
# only the dependencies, so this is the step that makes `import startorch` work.
RUN cd python && uv sync --frozen --no-dev

# ----------------------------------------------------------------------------
# Runtime Setup
# ----------------------------------------------------------------------------

FROM python:3.14-slim AS runtime
WORKDIR /app

# Copy python sources
# includes .venv and the built .so, no need to copy cpp
COPY --from=build /app/python /app/python          
COPY --from=build /app/project-config.toml /app/

# Copy front-end
COPY frontend/ frontend/

# Signal that we'll expose to port 8000
EXPOSE 8000

# Default args for run command
CMD ["/app/python/.venv/bin/uvicorn", "startorch.api.api:app", "--host", "0.0.0.0", "--port", "8000"]
