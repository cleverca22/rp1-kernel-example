{ stdenv, liburing }:

stdenv.mkDerivation {
  name = "userland-example";
  buildInputs = [ liburing ];
  src = ./.;
  dontStrip = true;
}
