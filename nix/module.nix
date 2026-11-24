# NixOS module: runs `qac serve` as a systemd service.
#
#   services.qac = {
#     enable = true;
#     port = 8080;
#     jamspellModel = "/var/lib/qac-models/multilingual.bin";   # optional
#   };
#
# The data directory is the unit's state directory, /var/lib/qac.
self:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.qac;
  inherit (lib) mkEnableOption mkIf mkOption types;
in
{
  options.services.qac = {
    enable = mkEnableOption "the qac query autocomplete server";

    package = mkOption {
      type = types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "qac.packages.\${system}.default";
      description = "The qac package to run.";
    };

    listenAddress = mkOption {
      type = types.str;
      default = "127.0.0.1";
      description = "Address to listen on.";
    };

    port = mkOption {
      type = types.port;
      default = 8080;
      description = "TCP port of the REST API.";
    };

    openFirewall = mkOption {
      type = types.bool;
      default = false;
      description = "Open the port in the firewall.";
    };

    jamspellModel = mkOption {
      type = types.nullOr types.path;
      default = null;
      description = "JamSpell language model; enables spell correction (correct=1 requests).";
    };

    extraArgs = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = [ "--workers" "8" "--fsync" "always" ];
      description = "Further arguments for `qac serve`.";
    };
  };

  config = mkIf cfg.enable {
    networking.firewall.allowedTCPPorts = mkIf cfg.openFirewall [ cfg.port ];

    systemd.services.qac = {
      description = "qac query autocomplete server";
      wantedBy = [ "multi-user.target" ];
      after = [ "network.target" ];

      serviceConfig = {
        ExecStart = lib.escapeShellArgs ([
          "${cfg.package}/bin/qac"
          "serve"
          "--data-dir"
          "/var/lib/qac"
          "--listen"
          cfg.listenAddress
          "--port"
          (toString cfg.port)
        ]
        ++ lib.optionals (cfg.jamspellModel != null) [ "--jamspell-model" (toString cfg.jamspellModel) ]
        ++ cfg.extraArgs);

        DynamicUser = true;
        StateDirectory = "qac";
        Restart = "on-failure";
        # SIGTERM triggers a final snapshot, which can take a while for big indexes.
        KillSignal = "SIGTERM";
        TimeoutStopSec = "10min";
        LimitNOFILE = 65536;
      };
    };
  };
}
