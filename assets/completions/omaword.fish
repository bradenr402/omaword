# fish completion for omaword: `omaword completion fish > ~/.config/fish/completions/omaword.fish`.

set -l commands open lookup quote search translations link import completion help
complete -c omaword -f
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a open -d 'Open a passage in a window'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a lookup -d 'Look up a passage in a small window'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a quote -d 'Print passages as quotations'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a search -d 'Search the Bible'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a translations -d 'List the installed translations'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a link -d 'Print an omaword: link to a passage'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a import -d 'Install a translation from AO complete JSON'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a completion -d 'Print a shell completion script'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -a help -d 'Show help for OmaWord or a command'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -s h -l help -d 'Show help'
complete -c omaword -n "not __fish_seen_subcommand_from $commands" -s v -l version -d "Show OmaWord's version"
complete -c omaword -n "__fish_seen_subcommand_from open lookup quote search link" -s t -l translation -x -a '(omaword translations --names 2>/dev/null)' -d 'Use a translation'
complete -c omaword -n "__fish_seen_subcommand_from open" -l here -d 'In the window you were reading'
complete -c omaword -n "__fish_seen_subcommand_from quote" -l plain -d 'The text alone'
complete -c omaword -n "__fish_seen_subcommand_from quote search translations" -l json -d 'JSON output'
complete -c omaword -n "__fish_seen_subcommand_from search" -l all -d 'Every installed translation'
complete -c omaword -n "__fish_seen_subcommand_from search" -l in -x -a 'ot nt apocrypha' -d 'Only these books: ot, nt, apocrypha, or a book'
complete -c omaword -n "__fish_seen_subcommand_from search" -l limit -x -d 'At most this many matches (0: all)'
complete -c omaword -n "__fish_seen_subcommand_from search" -l color -x -a 'auto always never' -d 'When to color matches'
complete -c omaword -n "__fish_seen_subcommand_from translations" -l names -d 'Abbreviations only'
complete -c omaword -n "__fish_seen_subcommand_from import" -F
complete -c omaword -n "__fish_seen_subcommand_from completion" -x -a 'bash zsh fish'
complete -c omaword -n "__fish_seen_subcommand_from help" -x -a "$commands"
