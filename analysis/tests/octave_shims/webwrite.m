function response = webwrite (url, data, varargin)
  % WEBWRITE  MATLAB-compatible stand-in for GNU Octave (tests only).
  %
  % Octave 8's own webwrite can only post form fields ("name=value&..."); it
  % rejects a JSON body. MATLAB's webwrite sends a character-vector DATA as it
  % is, with Content-Type taken from weboptions('MediaType', ...). This file
  % gives Octave that one behaviour so ast_nightly.m runs unchanged:
  %     response = webwrite (url, json_text, weboptions ('MediaType', 'application/json'))
  % The request is made with the curl program. Like MATLAB, an HTTP status of
  % 400 or more is an error and a JSON reply is returned decoded.
  if (nargin < 2 || ! ischar (url) || ! ischar (data))
    error ("webwrite (shim): usage webwrite (URL, CHAR_DATA [, weboptions])");
  endif
  media = "application/x-www-form-urlencoded";
  timeout = 15;
  if (! isempty (varargin) && isa (varargin{end}, "weboptions"))
    opts = varargin{end};
    if (! strcmp (opts.MediaType, "auto"))
      media = opts.MediaType;
    endif
    timeout = opts.Timeout;
  endif
  q = @(s) ["'", strrep(s, "'", "'\\''"), "'"];          % shell single-quoting
  cmd = sprintf ("curl -sS --noproxy 127.0.0.1,localhost --max-time %d -X POST -H %s --data-binary %s -w %s %s 2>&1", ...
                 ceil (timeout), q (["Content-Type: ", media]), q (data), q ('\n%{http_code}'), q (url));
  [status, out] = system (cmd);
  if (status != 0)
    error ("webwrite (shim): curl failed: %s", strtrim (out));
  endif
  cut = find (out == "\n", 1, "last");
  code = str2double (strtrim (out(cut+1:end)));
  body = out(1:cut-1);
  if (isnan (code) || code >= 400)
    error ("webwrite (shim): the server returned the status %d with the message \"%s\"", code, strtrim (body));
  endif
  response = body;
  try
    response = jsondecode (body);
  catch
  end_try_catch
endfunction
