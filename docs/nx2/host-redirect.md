# Testing against a server you control

Configure > Network > Host redirect sends selected connections to a server you run. Use it for
homebrew or other software and services you have permission to test. It does nothing while it's
off, and it's read when a game starts. This feature does not provide Nintendo service access or
authorization to use a third party's services.

## Settings

- **Host names**: separated by commas or whitespace. Each name covers itself and its
  subdomains, and a leading `*.` means the same. For example, `*.api.example.test
  games.example.test` redirects both test-domain families. These names resolve to 127.0.0.1.
- **Server address**: `<ip>:<port>`. TCP connections to 127.0.0.1:443 go there instead.
- **CA certificate**: one DER certificate of at most 64 KiB. The guest's TLS trusts it as well
  as the usual roots, for every connection. Issue your server's certificate from it, with the
  redirected names as subject alternative names, and the emulated SSL service accepts it.
  Use a dedicated test CA, not a production credential. This trust change is not limited to the
  redirected names, so keep unrelated network access blocked during testing.
- **Block all other network access** (loopback only): names that aren't redirected don't
  resolve, and the guest can reach only this PC and the server address.

## Id tokens

With the redirect and loopback only both on, nn::account answers `LoadIdTokenCache` with a
token Lichii makes for the profile the game asked about. Its `sub` is the profile's id (32 hex
digits), it carries the title version (`tv`), and it has no Nextendo account or nx2 token
(`nnex`). It's signed with a key made for the session, not a Nintendo or Nextendo signing key.
It is a test identity, not proof of an account or entitlement. Only accept it in an isolated test
service you control; never use it to authorize access to production data or services. This test
mode does not require a Nextendo sign-in or a Nextendo catalogue entry. Loopback only restricts
guest connections to this PC and the configured server address, which need not be local.

Without loopback only, games keep their normal Nextendo token.

## Redirect-only mods

Software can check certificates or host names independently of the emulated SSL service. For
software you develop, configure its test build to trust your test server. Adding a CA here does
not override those independent checks.

The `[Host redirect]` mod-folder prefix scopes an existing development mod to sessions with
Host redirect enabled. Ordinary IPS or IPSwitch mods use `load/<title id>/<mod>/exefs/`; for
example, a development mod can be named `[Host redirect] Local server`. The log warns for each
such mod it applies. Lichii does not bundle these redirect-only mods. Use only modifications you
have permission to make and share; this guide does not provide patches for third-party access
controls.
