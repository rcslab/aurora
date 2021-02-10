<?php


final class ClangFormatReplacement {
  public $offset, $length, $replacement;
  public $line, $char;

  public function __construct($offset, $length, $replacement)
  {
    $this->offset = $offset;
    $this->length = $length;
    $this->replacement = $replacement;
  }
}

/**
 * Uses the clang format to format C/C++/Obj-C code
 */
final class ClangFormatLinter extends ArcanistExternalLinter {

  public function getInfoName() {
    return 'clang-format';
  }

  public function getInfoURI() {
    return '';
  }

  public function getInfoDescription() {
    return pht('Use clang-format for processing specified files.');
  }

  public function getLinterName() {
    return 'clang-format';
  }

  public function getLinterConfigurationName() {
    return 'clang-format';
  }

  public function getLinterConfigurationOptions() {
    return parent::getLinterConfigurationOptions();
  }

  public function getDefaultBinary() {
    return 'clang-format';
  }

  public function getInstallInstructions() {
    return pht('Make sure clang-format is in directory specified by $PATH');
  }

  public function shouldExpectCommandErrors() {
    return false;
  }

  protected function getMandatoryFlags() {
    $options = array('-output-replacements-xml');

    return $options;
  }

  public function getVersion() {
    list($stdout, $stderr) = execx(
      '%C --version', $this->getExecutableCommand());
    $matches = null;
    if (preg_match('/clang-format version (\d(?:\.\d){2})/', $stdout, $matches)) {
      return $matches[1];
    }
    return null;
  }

  protected function parseLinterOutput($path, $err, $stdout, $stderr) {
    $messages = array();
    $errors = array();

    if ($err != 0) {
      return $messages;
    }

    /* Open original file */
    $root = $this->getProjectRoot();
    $path = Filesystem::resolvePath($path, $root);
    $orig = file_get_contents($path);
    /* Last parameter = true to retain line ending */
    $lines = phutil_split_lines($orig, true);

    $replacements = new SimpleXMLElement($stdout);

    foreach ($replacements->replacement as $rep) {
      $errors[] = new ClangFormatReplacement(intval($rep['offset']), intval($rep['length']), strval($rep));
    }

    if (count($errors) == 0) {
      return $messages;
    }

    $cur_error = 0;
    $error = $errors[0];
    $line_num = 1;
    $char_count = 0;

    /* iterate on each file lines and match the errors to the current offset */
    foreach($lines as $line) {

      $line_length = strlen($line);

      /* There may be multiple errors per lines, iterate until it matches */
      while ($error->offset >= $char_count && $error->offset < ($char_count + $line_length)) {
        /* Offset start from 1 in arcanist but 0 in clang-format, add 1 */
        $line_char = $error->offset - $char_count + 1;

        $message = id(new ArcanistLintMessage())
          ->setPath($path)
          ->setLine($line_num)
          ->setChar($line_char)
          ->setCode('CFMT')
          ->setSeverity(ArcanistLintSeverity::SEVERITY_AUTOFIX)
          ->setName('Code style violation')
          ->setDescription("code style errors.")
          ->setOriginalText(substr($orig, $error->offset, $error->length))
          ->setReplacementText($error->replacement);

        $messages[] = $message;

        /* Next error */
        $cur_error += 1;

        /* We might have found all errors, early exit */
        if ($cur_error == count($errors)) {
          break;
        }

        $error = $errors[$cur_error];
      }
      /* We also ned to break out of this loop */
      if ($cur_error == count($errors)) {
        break;
      }

      $char_count = $char_count + $line_length;
      $line_num += 1;
    }

    return $messages;
  }
}
