# Naming builds for the owner to test

Every build the owner has to test, when it comes from Kiro's changes, gets a name that starts with `kiro`
(for example `kiro-<short description>-<short sha>`): the commit's branch or tag, and the name given to the
owner in the chat. Give that name every time a build is requested, with the Actions run link.

The `Build Windows` workflow uploads its artifacts as `kiro-orbitals-recomp-windows-x64` (full package) and
`kiro-orbitals-recomp-exe` (executable only).
