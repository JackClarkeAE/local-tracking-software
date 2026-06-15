#!/usr/bin/env python3
"""
Small standalone OpenEMR FHIR/REST probe.

This script is intentionally separate from the C++ app. Use it to prove:
1. OpenEMR API/FHIR endpoints are enabled and reachable.
2. OAuth2/OpenID Connect discovery works.
3. A SMART/OAuth Authorization Code + PKCE login can produce an access token.
4. The token can read a simple FHIR resource, such as Patient.

Examples:
  python3 tools/openemr_fhir_probe.py discover --base-url https://localhost:9300 --insecure

  python3 tools/openemr_fhir_probe.py auth \
    --base-url https://localhost:9300 \
    --client-id YOUR_CLIENT_ID \
    --client-secret YOUR_CLIENT_SECRET \
    --insecure

  python3 tools/openemr_fhir_probe.py patient-search \
    --base-url https://localhost:9300 \
    --client-id YOUR_CLIENT_ID \
    --patient-name Smith \
    --insecure
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.server
import json
import secrets
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from dataclasses import dataclass
from typing import Any


DEFAULT_SCOPE = "openid fhirUser offline_access user/Patient.rs user/Observation.rs"


@dataclass
class Endpoints:
    base_url: str
    site: str
    fhir_base: str
    oauth_base: str
    smart_config: str
    oidc_config: str
    capability_statement: str


def normalize_url(url: str) -> str:
    return url.rstrip("/")


def build_endpoints(base_url: str, site: str, fhir_base: str | None, oauth_base: str | None) -> Endpoints:
    base_url = normalize_url(base_url)
    fhir = normalize_url(fhir_base) if fhir_base else f"{base_url}/apis/{site}/fhir"
    oauth = normalize_url(oauth_base) if oauth_base else f"{base_url}/oauth2/{site}"
    return Endpoints(
        base_url=base_url,
        site=site,
        fhir_base=fhir,
        oauth_base=oauth,
        smart_config=f"{fhir}/.well-known/smart-configuration",
        oidc_config=f"{oauth}/.well-known/openid-configuration",
        capability_statement=f"{fhir}/metadata",
    )


def ssl_context(insecure: bool) -> ssl.SSLContext | None:
    if not insecure:
        return None
    return ssl._create_unverified_context()


def http_request(
    method: str,
    url: str,
    *,
    headers: dict[str, str] | None = None,
    body: bytes | None = None,
    insecure: bool = False,
) -> tuple[int, dict[str, str], Any]:
    req = urllib.request.Request(url, data=body, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(req, context=ssl_context(insecure), timeout=30) as resp:
            raw = resp.read()
            return resp.status, dict(resp.headers.items()), parse_body(raw)
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        return exc.code, dict(exc.headers.items()), parse_body(raw)


def parse_body(raw: bytes) -> Any:
    if not raw:
        return None
    text = raw.decode("utf-8", errors="replace")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def print_json(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def safe_b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def make_pkce_pair() -> tuple[str, str]:
    verifier = safe_b64url(secrets.token_bytes(64))
    challenge = safe_b64url(hashlib.sha256(verifier.encode("ascii")).digest())
    return verifier, challenge


class OAuthCallbackHandler(http.server.BaseHTTPRequestHandler):
    server_version = "OpenEMRFHIRProbe/1.0"

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        self.server.auth_result = {key: values[0] for key, values in params.items()}  # type: ignore[attr-defined]
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(
            b"<html><body><h1>OpenEMR probe received the callback.</h1>"
            b"<p>You can close this browser tab and return to the terminal.</p></body></html>"
        )

    def log_message(self, fmt: str, *args: Any) -> None:
        return


def wait_for_callback(redirect_uri: str, timeout_seconds: int) -> dict[str, str]:
    parsed = urllib.parse.urlparse(redirect_uri)
    if parsed.scheme != "http" or parsed.hostname not in ("127.0.0.1", "localhost"):
        raise ValueError("This probe only hosts local http://127.0.0.1 or http://localhost redirect URIs")

    port = parsed.port or 80
    server = http.server.HTTPServer((parsed.hostname, port), OAuthCallbackHandler)
    server.timeout = 1
    server.auth_result = None  # type: ignore[attr-defined]

    deadline = time.time() + timeout_seconds
    while time.time() < deadline and server.auth_result is None:  # type: ignore[attr-defined]
        server.handle_request()
        if server.auth_result is not None:  # type: ignore[attr-defined]
            break

    result = server.auth_result  # type: ignore[attr-defined]
    server.server_close()
    if not result:
        raise TimeoutError(f"No OAuth callback received at {redirect_uri} within {timeout_seconds}s")
    return result


def discover(args: argparse.Namespace) -> dict[str, Any]:
    endpoints = build_endpoints(args.base_url, args.site, args.fhir_base, args.oauth_base)
    checks = {}

    for name, url in (
        ("fhir_capability_statement", endpoints.capability_statement),
        ("smart_configuration", endpoints.smart_config),
        ("oidc_configuration", endpoints.oidc_config),
    ):
        print(f"\n== {name}: {url}")
        status, _, body = http_request(
            "GET",
            url,
            headers={"Accept": "application/fhir+json, application/json"},
            insecure=args.insecure,
        )
        checks[name] = {"url": url, "status": status, "body": body}
        print(f"HTTP {status}")
        if isinstance(body, dict):
            summary = summarize_discovery_body(name, body)
            print_json(summary)
        else:
            print(body)

    return checks


def summarize_discovery_body(name: str, body: dict[str, Any]) -> dict[str, Any]:
    if name == "fhir_capability_statement":
        resources = []
        for rest in body.get("rest", []):
            for resource in rest.get("resource", []):
                resource_type = resource.get("type")
                if resource_type:
                    resources.append(resource_type)
        return {
            "resourceType": body.get("resourceType"),
            "fhirVersion": body.get("fhirVersion"),
            "software": body.get("software"),
            "resources_sample": resources[:25],
            "resource_count": len(resources),
        }
    return {
        "issuer": body.get("issuer"),
        "authorization_endpoint": body.get("authorization_endpoint"),
        "token_endpoint": body.get("token_endpoint"),
        "registration_endpoint": body.get("registration_endpoint"),
        "scopes_supported_sample": body.get("scopes_supported", [])[:25],
        "grant_types_supported": body.get("grant_types_supported"),
        "code_challenge_methods_supported": body.get("code_challenge_methods_supported"),
    }


def get_oidc_config(args: argparse.Namespace) -> dict[str, Any]:
    endpoints = build_endpoints(args.base_url, args.site, args.fhir_base, args.oauth_base)
    status, _, body = http_request(
        "GET",
        endpoints.oidc_config,
        headers={"Accept": "application/json"},
        insecure=args.insecure,
    )
    if status >= 400 or not isinstance(body, dict):
        raise RuntimeError(f"Could not load OIDC config from {endpoints.oidc_config}: HTTP {status} {body!r}")
    return body


def register_client(args: argparse.Namespace) -> None:
    oidc = get_oidc_config(args)
    registration_endpoint = oidc.get("registration_endpoint")
    if not registration_endpoint:
        raise RuntimeError("OIDC configuration did not include registration_endpoint")

    payload = {
        "application_type": args.application_type,
        "client_name": args.client_name,
        "redirect_uris": [args.redirect_uri],
        "scope": args.scope,
        "grant_types": ["authorization_code", "refresh_token"],
        "response_types": ["code"],
        "token_endpoint_auth_method": args.token_endpoint_auth_method,
    }
    status, _, body = http_request(
        "POST",
        registration_endpoint,
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        body=json.dumps(payload).encode("utf-8"),
        insecure=args.insecure,
    )
    print(f"HTTP {status}")
    print_json(body)
    if status >= 400:
        print("\nRegistration failed. If OpenEMR requires manual approval, create the client in OpenEMR admin instead.")


def authorize(args: argparse.Namespace) -> dict[str, Any]:
    oidc = get_oidc_config(args)
    authorization_endpoint = oidc.get("authorization_endpoint")
    token_endpoint = oidc.get("token_endpoint")
    if not authorization_endpoint or not token_endpoint:
        raise RuntimeError("OIDC configuration did not include authorization_endpoint and token_endpoint")

    verifier, challenge = make_pkce_pair()
    state = secrets.token_urlsafe(24)
    auth_params = {
        "response_type": "code",
        "client_id": args.client_id,
        "redirect_uri": args.redirect_uri,
        "scope": args.scope,
        "state": state,
        "code_challenge": challenge,
        "code_challenge_method": "S256",
        "aud": build_endpoints(args.base_url, args.site, args.fhir_base, args.oauth_base).fhir_base,
    }
    auth_url = f"{authorization_endpoint}?{urllib.parse.urlencode(auth_params)}"

    print("\nOpen this authorization URL if the browser does not open automatically:\n")
    print(auth_url)
    print()

    if not args.no_browser:
        threading.Thread(target=lambda: webbrowser.open(auth_url), daemon=True).start()

    callback = wait_for_callback(args.redirect_uri, args.callback_timeout)
    if callback.get("state") != state:
        raise RuntimeError("OAuth state mismatch; refusing to exchange code")
    if "error" in callback:
        raise RuntimeError(f"Authorization error: {callback}")
    code = callback.get("code")
    if not code:
        raise RuntimeError(f"OAuth callback did not contain a code: {callback}")

    form = {
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": args.redirect_uri,
        "code_verifier": verifier,
    }
    headers = {
        "Content-Type": "application/x-www-form-urlencoded",
        "Accept": "application/json",
    }

    if args.client_secret and args.client_auth_method == "client_secret_basic":
        credentials = f"{args.client_id}:{args.client_secret}".encode("utf-8")
        headers["Authorization"] = f"Basic {base64.b64encode(credentials).decode('ascii')}"
    else:
        form["client_id"] = args.client_id

    if args.client_secret and args.client_auth_method == "client_secret_post":
        form["client_secret"] = args.client_secret
    elif args.client_auth_method == "client_secret_basic" and not args.client_secret:
        raise RuntimeError("--client-auth-method client_secret_basic requires --client-secret")

    status, _, body = http_request(
        "POST",
        token_endpoint,
        headers=headers,
        body=urllib.parse.urlencode(form).encode("utf-8"),
        insecure=args.insecure,
    )
    print(f"Token exchange HTTP {status}")
    if status >= 400:
        print_json(body)
        raise RuntimeError("Token exchange failed")
    if not isinstance(body, dict) or "access_token" not in body:
        print_json(body)
        raise RuntimeError("Token response did not contain access_token")

    printable = dict(body)
    if "access_token" in printable:
        printable["access_token"] = printable["access_token"][:16] + "...redacted"
    if "refresh_token" in printable:
        printable["refresh_token"] = printable["refresh_token"][:16] + "...redacted"
    print_json(printable)
    return body


def patient_search(args: argparse.Namespace) -> None:
    token = args.access_token
    if not token:
        token = authorize(args)["access_token"]

    endpoints = build_endpoints(args.base_url, args.site, args.fhir_base, args.oauth_base)
    params: dict[str, str] = {}
    if args.patient_identifier:
        params["identifier"] = args.patient_identifier
    if args.patient_name:
        params["name"] = args.patient_name

    url = f"{endpoints.fhir_base}/Patient"
    if params:
        url = f"{url}?{urllib.parse.urlencode(params)}"

    status, _, body = http_request(
        "GET",
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/fhir+json",
        },
        insecure=args.insecure,
    )
    print(f"\nPatient search: {url}")
    print(f"HTTP {status}")
    print_json(summarize_patient_bundle(body) if isinstance(body, dict) else body)


def summarize_patient_bundle(body: dict[str, Any]) -> dict[str, Any]:
    entries = body.get("entry", [])
    patients = []
    for entry in entries[:10]:
        resource = entry.get("resource", {})
        name = resource.get("name", [{}])[0]
        patients.append(
            {
                "id": resource.get("id"),
                "family": name.get("family"),
                "given": name.get("given"),
                "birthDate": resource.get("birthDate"),
                "gender": resource.get("gender"),
            }
        )
    return {
        "resourceType": body.get("resourceType"),
        "type": body.get("type"),
        "total": body.get("total"),
        "patients_sample": patients,
    }


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--base-url", required=True, help="OpenEMR base URL, for example https://localhost:9300")
    parser.add_argument("--site", default="default", help="OpenEMR site id; default: default")
    parser.add_argument("--fhir-base", help="Override full FHIR base URL")
    parser.add_argument("--oauth-base", help="Override full OAuth base URL")
    parser.add_argument("--insecure", action="store_true", help="Disable TLS certificate verification for local test systems")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Probe OpenEMR FHIR/OAuth connectivity")
    sub = parser.add_subparsers(dest="command", required=True)

    discover_parser = sub.add_parser("discover", help="Check FHIR metadata, SMART config, and OIDC config")
    add_common_args(discover_parser)

    register_parser = sub.add_parser("register", help="Attempt dynamic OAuth client registration")
    add_common_args(register_parser)
    register_parser.add_argument("--client-name", default="Local Tracking Software Probe")
    register_parser.add_argument("--redirect-uri", default="http://127.0.0.1:8765/callback")
    register_parser.add_argument("--scope", default=DEFAULT_SCOPE)
    register_parser.add_argument("--application-type", default="private", choices=("private", "public"))
    register_parser.add_argument(
        "--token-endpoint-auth-method",
        default="client_secret_post",
        choices=("client_secret_post", "client_secret_basic"),
    )

    auth_parser = sub.add_parser("auth", help="Run Authorization Code + PKCE and print redacted token response")
    add_common_args(auth_parser)
    auth_parser.add_argument("--client-id", required=True)
    auth_parser.add_argument("--client-secret", help="Only if your OpenEMR client is confidential")
    auth_parser.add_argument(
        "--client-auth-method",
        default="client_secret_post",
        choices=("client_secret_post", "client_secret_basic"),
    )
    auth_parser.add_argument("--redirect-uri", default="http://127.0.0.1:8765/callback")
    auth_parser.add_argument("--scope", default=DEFAULT_SCOPE)
    auth_parser.add_argument("--no-browser", action="store_true")
    auth_parser.add_argument("--callback-timeout", type=int, default=180)

    patient_parser = sub.add_parser("patient-search", help="Search FHIR Patient using an access token or interactive login")
    add_common_args(patient_parser)
    patient_parser.add_argument("--access-token", help="Use an existing token instead of logging in")
    patient_parser.add_argument("--client-id", help="Required if --access-token is not supplied")
    patient_parser.add_argument("--client-secret", help="Only if your OpenEMR client is confidential")
    patient_parser.add_argument(
        "--client-auth-method",
        default="client_secret_post",
        choices=("client_secret_post", "client_secret_basic"),
    )
    patient_parser.add_argument("--redirect-uri", default="http://127.0.0.1:8765/callback")
    patient_parser.add_argument("--scope", default=DEFAULT_SCOPE)
    patient_parser.add_argument("--no-browser", action="store_true")
    patient_parser.add_argument("--callback-timeout", type=int, default=180)
    patient_parser.add_argument("--patient-name", help="FHIR Patient name search")
    patient_parser.add_argument("--patient-identifier", help="FHIR Patient identifier search")

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        if args.command == "discover":
            discover(args)
        elif args.command == "register":
            register_client(args)
        elif args.command == "auth":
            authorize(args)
        elif args.command == "patient-search":
            if not args.access_token and not args.client_id:
                parser.error("patient-search needs --access-token or --client-id")
            patient_search(args)
        else:
            parser.error(f"Unknown command: {args.command}")
        return 0
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
