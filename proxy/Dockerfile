FROM python:3.12-alpine
WORKDIR /app
COPY claude_usage_proxy.py .
# Credentials werden als Volume gemountet, nie ins Image gebacken
ENV CREDENTIALS_FILE=/data/.credentials.json
EXPOSE 8787
CMD ["python", "claude_usage_proxy.py"]
