<?php

/**
 * @file
 * Defines functions used when constructing derivatives.
 */

use Drupal\Component\Utility\Xss;
use Drupal\Core\Logger\RfcLogLevel;
use Drupal\Core\Link;

// Relations.
define('ISLANDORA_DEFER_DERIVATIVES_FLAG', 'deferDerivatives');

/**
 * Decides which derivative function to call and runs it.
 *
 * @param AbstractObject $object
 *   The object to run the derivative function for.
 * @param string $dsid
 *   The DSID to run the derivative function for.
 */
function islandora_run_derivatives(AbstractObject $object, $dsid) {
  $batch_array = batch_get();
  if (empty($batch_array)) {
    $logging_results = islandora_do_derivatives($object, [
      'source_dsid' => $dsid,
    ]);
    islandora_derivative_logging($logging_results);
  }
  else {
    $operations = islandora_do_batch_derivatives(
      $object,
      [
        'source_dsid' => $dsid,
      ]
    );
    if ($operations) {
      batch_set(
        // Title won't show for batch in a batch.
        [
          'init_message' => t('Preparing derivatives for @label', ['@label' => $object->label]),
          'error_message' => t('An error occured creating derivatives.'),
          'progress_message' => t(
            'Creating derivatives for @label <br/>Time elapsed: @elapsed <br/>
              Estimated time remaining @estimate.',
            ['@label' => $object->label]
          ),
          'file' => drupal_get_path('module', 'islandora') . '/includes/regenerate_derivatives.form.inc',
          'operations' => $operations,
        ]
      );
    }
  }
}

/**
 * Kicks off derivative functions based upon hooks and conditions.
 *
 * @param AbstractObject $object
 *   An AbstractObject representing a FedoraObject.
 * @param array $options
 *   An array of parameters containing:
 *   - force: Bool denoting whether we are forcing the generation of
 *     derivatives.
 *   - source_dsid: (Optional) String of the datastream id we are generating
 *     from or NULL if it's the object itself.
 *   - destination_dsid: (Optional) String of the datastream id that is being
 *     created. To be used in the UI.
 *   - ds_modified_params: (Optional, typically internally used) The array of
 *     parameters originally sent to FedoraDatastream::modifyDatastream(). Used
 *     to refine the derivative list; typically should't be passed in manually.
 *
 * @return array
 *   An array of messages describing the outcome of the derivative events.
 *   Each individual message array has the following structure:
 *   - success: Bool denoting whether the operation was successful.
 *   - messages: An array structure containing:
 *     - message: A string passed through t() describing the
 *       outcome of the operation.
 *     - message_sub: (Optional) Substitutions to be passed along to t() or
 *       the logger.
 *     - type: A string denoting whether the output is to be
 *       drupal_set_messaged (dsm) or logged (log).
 *     - severity: (Optional) A severity level / status to be used when
 *       logging messages. Uses the defaults of drupal_set_message and
 *       the logger if not defined.
 */
function islandora_do_derivatives(AbstractObject $object, array $options) {
  $results = [];
  $derivatives = islandora_get_derivative_list($object, $options);
  foreach ($derivatives as $derivative) {
    if (isset($derivative['file'])) {
      require_once $derivative['file'];
    }
    foreach ($derivative['function'] as $function) {
      if (function_exists($function)) {
        $logging = call_user_func($function, $object, $options['force'], $derivative);
        if (!empty($logging)) {
          $results[] = $logging;
        }
      }
      else {
        \Drupal::logger('islandora')->error(
          'Unable to call derivative function @function as it was not found!',
          ['@function' => $function]
        );
      }
    }
  }
  return $results;
}

/**
 * Handles the logging of derivative messages.
 *
 * @param array $logging_results
 *   An array of messages describing the outcome of the derivative events.
 *   Each individual message array has the following structure:
 *   - success: Bool denoting whether the operation was successful.
 *   - messages: An array structure containing:
 *     - message: A string passed through t() describing the
 *       outcome of the operation.
 *     - message_sub: (Optional) Substitutions to be passed along to t() or
 *       the logger.
 *     - type: A string denoting whether the output is to be
 *       drupal_set_messaged (dsm) or logged (log).
 *     - severity: (Optional) A severity level / status to be used when
 *       logging messages. Uses the defaults of drupal_set_message and
 *       log if not defined.
 */
function islandora_derivative_logging(array $logging_results) {
  $result_messages = [
    'success' => Link::createFromRoute(
      t('Derivatives successfully created'),
      'islandora.event_status'
    )->toString(),
    'incomplete' => Link::createFromRoute(
      t('Not all derivatives successfully created.  Please check log messages'),
      'islandora.event_status'
    )->toString(),
  ];
  foreach ($logging_results as $result) {
    foreach ($result['messages'] as $message) {
      $result_message = $result_messages['success'];
      $message_status = 'status';
      if ($message['type'] === 'dsm') {
        if (isset($message['severity']) && $message['severity'] != 'status') {
          drupal_set_message(Xss::filter(format_string($message['message'], isset($message['message_sub']) ? $message['message_sub'] : [])), $message['severity']);
        }
        else {
          if (!isset($_SESSION['islandora_event_messages'])) {
            $_SESSION['islandora_event_messages'] = [];
          }
          $_SESSION['islandora_event_messages'][] = [
            'message' => Xss::filter(format_string($message['message'], isset($message['message_sub']) ? $message['message_sub'] : [])),
            'severity' => 'status',
          ];
        }
      }
      else {
        \Drupal::logger('islandora_derivatives')->log(
          isset($message['severity']) ? $message['severity'] : RfcLogLevel::NOTICE,
          $message['message'],
          isset($message['message_sub']) ? $message['message_sub'] : []
        );
        $result_message = $result_messages['incomplete'];
        $message_status = 'warning';
      }
      drupal_set_message(
        $result_message,
        $message_status,
        FALSE
      );
      if (isset($_SESSION['_symfony_flashes']['warning']) && isset($_SESSION['_symfony_flashes']['status'])) {
        unset($_SESSION['_symfony_flashes']['status']);
      }
    }
  }
}

/**
 * Kicks off derivative functions based upon hooks and conditions.
 *
 * @param AbstractObject $object
 *   An AbstractObject representing a FedoraObject.
 * @param array $options
 *   An array of parameters containing:
 *   - force: Bool denoting whether we are forcing the generation of
 *     derivatives.
 *   - source_dsid: (Optional) String of the datastream id we are generating
 *     from or NULL if it's the object itself.
 *   - destination_dsid: (Optional) String of the datastream id that is being
 *     created. To be used in the UI.
 *
 * @return array
 *   An array of operations to be called from within a batch.
 */
function islandora_do_batch_derivatives(AbstractObject $object, array $options) {
  $operations = [];
  $derivatives = islandora_get_derivative_list($object, $options);
  foreach ($derivatives as $derivative) {
    $file = FALSE;
    if (isset($derivative['file'])) {
      $file = $derivative['file'];
    }
    foreach ($derivative['function'] as $function) {
      $operations[] = [
        'islandora_derivative_perform_batch_operation',
        [
          $function,
          $file,
          $object->id,
          $options['force'],
          $derivative,
        ],
      ];
    }
  }
  return $operations;
}

/**
 * Filter the derivative functions to only call those which are valid.
 *
 * @param array $hooks
 *   An array of hooks to be filtered depending on options.
 * @param array $options
 *   An array of options for the derivative generation.
 * @param AbstractObject $object
 *   An AbstractObject representing an object within Fedora.
 *
 * @return array
 *   Returns the filtered array of hooks to be ran.
 */
function islandora_filter_derivatives(array $hooks, array $options, AbstractObject $object) {
  if (array_key_exists('source_dsid', $options)) {
    $hooks = array_filter($hooks, function ($filter_hook) use ($options) {
      return array_key_exists('source_dsid', $filter_hook) &&
      $filter_hook['source_dsid'] == $options['source_dsid'];
    });
  }
  if (array_key_exists('destination_dsid', $options)) {
    $hooks = array_filter($hooks, function ($filter_hook) use ($options) {
      return array_key_exists('destination_dsid', $filter_hook) &&
      $filter_hook['destination_dsid'] == $options['destination_dsid'];
    });
  }
  // Do a final filtering to make sure that the source DSID exists on the object
  // where needed. Using a defined function as opposed to the way above as
  // it seems to break PHPCS as of 1.4.8.
  $filter_function = function ($filter_hook) use ($object) {
    $to_return = FALSE;
    if (array_key_exists('source_dsid', $filter_hook)) {
      if ($filter_hook['source_dsid'] != NULL) {
        if (isset($object[$filter_hook['source_dsid']])) {
          $to_return = TRUE;
        }
      }
      else {
        $to_return = TRUE;
      }
    }
    return $to_return;
  };
  $hooks = array_filter($hooks, $filter_function);
  return $hooks;
}

/**
 * Set the defer derivatives flag on an object.
 */
function islandora_set_defer_derivatives_flag(AbstractObject $object) {
  $object->relationships->add(
    ISLANDORA_RELS_EXT_URI,
    ISLANDORA_DEFER_DERIVATIVES_FLAG,
    'true',
    RELS_TYPE_PLAIN_LITERAL
  );
}

/**
 * Get the defer derivatives flag on an object.
 */
function islandora_get_defer_derivatives_flag(AbstractObject $object) {
  return $object->relationships->get(
    ISLANDORA_RELS_EXT_URI,
    ISLANDORA_DEFER_DERIVATIVES_FLAG,
    'true',
    RELS_TYPE_PLAIN_LITERAL
  );
}

/**
 * Remove the defer derivatives flag on an object.
 */
function islandora_remove_defer_derivatives_flag(AbstractObject $object) {
  $object->relationships->remove(
    ISLANDORA_RELS_EXT_URI,
    ISLANDORA_DEFER_DERIVATIVES_FLAG,
    'true',
    RELS_TYPE_PLAIN_LITERAL
  );
}

/**
 * Get the list of derivatives.
 *
 * @param AbstractObject $object
 *   The object to find derivatives for.
 * @param array $options
 *   Options for derivatives, will be updated to default force to FALSE.
 */
function islandora_get_derivative_list(AbstractObject $object, array &$options) {
  module_load_include('inc', 'islandora', 'includes/utilities');

  $ds_modified_params = isset($options['ds_modified_params']) ? $options['ds_modified_params'] : [];
  $options += [
    'force' => FALSE,
  ];

  $derivatives = islandora_invoke_hook_list(
    ISLANDORA_DERIVATIVE_CREATION_HOOK,
    $object->models,
    [$object],
    $ds_modified_params
  );

  foreach (islandora_build_hook_list(ISLANDORA_DERIVATIVE_CREATION_HOOK, $object->models) as $hook) {
    \Drupal::moduleHandler()->alter($hook, $derivatives, $object, $ds_modified_params);
  }

  uasort($derivatives, ['Drupal\Component\Utility\SortArray', 'sortByWeightElement']);
  return islandora_filter_derivatives($derivatives, $options, $object);
}
