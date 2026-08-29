# Faultline Control Lab

The dashboard is a dependency-free static client for the Faultline control API.

```sh
python3 -m http.server 4173 --directory ui
```

Open `http://127.0.0.1:4173`, then set the control endpoint and optional bearer token from the settings button.

The regular container stack starts with `docker compose up --build`. The self-contained demo starts with `./scripts/demo.sh`. In both modes the dashboard uses its same-origin `/api` reverse proxy automatically.
