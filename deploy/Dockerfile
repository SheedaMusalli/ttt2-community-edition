FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    TTT2_RANKED_DB=/data/ranked.sqlite3 \
    TTT2_RANKED_HOST=0.0.0.0 \
    TTT2_RANKED_PORT=8765

WORKDIR /app
RUN addgroup --system --gid 10001 ranked \
    && adduser --system --uid 10001 --ingroup ranked --home /nonexistent ranked \
    && mkdir /data \
    && chown ranked:ranked /data
COPY port/tools/ranked_service.py /app/ranked_service.py
USER 10001:10001
EXPOSE 8765
HEALTHCHECK --interval=20s --timeout=3s --start-period=5s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/healthz', timeout=2).read()"
ENTRYPOINT ["python", "/app/ranked_service.py"]
CMD ["serve"]
