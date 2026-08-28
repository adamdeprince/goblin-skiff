completionsdir = @completions@
dist_completions_DATA =
nobase_dist_sysconf_DATA =

if INSTALL_UFW
  nobase_dist_sysconf_DATA += ufw/applications.d/adam-mosh
endif

if INSTALL_COMPLETION
  dist_completions_DATA += bash-completion/completions/adam-mosh
endif
