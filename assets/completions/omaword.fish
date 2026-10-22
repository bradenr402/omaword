# fish completion for omaword: `omaword completion fish > ~/.config/fish/completions/omaword.fish`.

set -l commands open lookup read quote search translations link import completion help
set -l given $commands versions
complete -c omaword -f
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a open -d 'Open a passage in a window'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a lookup -d 'Look up a passage in a small window'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a read -d 'Print passages as the reader lays them out'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a quote -d 'Print passages as quotations'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a search -d 'Search the Bible'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a translations -d 'List the installed translations'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a link -d 'Print an omaword: link to a passage'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a import -d 'Install a translation from AO complete JSON'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a completion -d 'Print a shell completion script'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a help -d 'Show help for OmaWord or a command'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -s h -l help -d 'Show help'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -s v -l version -d "Show OmaWord's version"
# A group's commands follow its name (after help too).
complete -c omaword -n "__fish_seen_subcommand_from translations versions; and not __fish_seen_subcommand_from list" -a list -d 'List the installed translations'
complete -c omaword -n "__fish_seen_subcommand_from open lookup read quote search link" -s t -l translation -x -a '(omaword translations --names 2>/dev/null)' -d 'Use a translation'
complete -c omaword -n "__fish_seen_subcommand_from open" -l here -d 'In the window you were reading'
complete -c omaword -n "__fish_seen_subcommand_from read" -l footnotes -d 'The footnotes after each passage'
complete -c omaword -n "__fish_seen_subcommand_from quote" -l plain -d 'The text alone'
complete -c omaword -n "__fish_seen_subcommand_from read quote search translations versions" -l json -d 'JSON output'
complete -c omaword -n "__fish_seen_subcommand_from quote search" -l all -d 'Every installed translation'
complete -c omaword -n "__fish_seen_subcommand_from search" -l in -x -a 'ot nt apocrypha' -d 'Only these books: ot, nt, apocrypha, or a book'
complete -c omaword -n "__fish_seen_subcommand_from search" -l limit -x -d 'At most this many matches (0: all)'
complete -c omaword -n "__fish_seen_subcommand_from read search" -l color -x -a 'auto always never' -d 'When to color'
complete -c omaword -n "__fish_seen_subcommand_from translations versions" -l names -d 'Abbreviations only'
complete -c omaword -n "__fish_seen_subcommand_from import" -F
complete -c omaword -n "__fish_seen_subcommand_from completion" -x -a 'bash zsh fish'
complete -c omaword -n "__fish_seen_subcommand_from help; and not __fish_seen_subcommand_from $commands[1..-2]" -x -a "$commands"
