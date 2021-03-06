<?php

namespace Drupal\islandora\Controller;

use Drupal\Core\Access\AccessResult;
use Drupal\Core\Controller\ControllerBase;
use Drupal\Core\Session\AccountInterface;
use Drupal\Core\Url;
use Drupal\Core\Render\RendererInterface;
use Drupal\Core\Cache\CacheableMetadata;

use Drupal\islandora\Form\SolutionPackForm;

use Symfony\Component\DependencyInjection\ContainerInterface;
use Symfony\Component\HttpKernel\Exception\NotFoundHttpException;
use Symfony\Component\HttpFoundation\BinaryFileResponse;
use Symfony\Component\HttpFoundation\StreamedResponse;
use Symfony\Component\HttpFoundation\RedirectResponse;
use Drupal\Core\Cache\CacheableJsonResponse as JsonResponse;
use Symfony\Component\HttpFoundation\Request;

use AbstractObject;
use AbstractDatastream;

/**
 * Class DefaultController.
 *
 * @package Drupal\islandora\Controller
 */
class DefaultController extends ControllerBase {
  const LISTING_TAG = 'islandora_listing';

  protected $renderer;

  protected $appRoot;

  /**
   * Constructor for dependency injection.
   */
  public function __construct(RendererInterface $renderer, $appRoot) {
    $this->renderer = $renderer;
    $this->appRoot = $appRoot;
  }

  /**
   * Dependency Injection.
   */
  public static function create(ContainerInterface $container) {
    return new static(
      $container->get('renderer'),
      $container->get('app.root')
    );
  }

  /**
   * Administer solutions packs.
   *
   * @return array
   *   Renderable for the solution pack administration page.
   *
   * @throws \Exception
   */
  public function solutionPacksAdmin() {
    module_load_include('inc', 'islandora', 'includes/utilities');
    module_load_include('inc', 'islandora', 'includes/solution_packs');

    if (!islandora_describe_repository()) {
      islandora_display_repository_inaccessible_message();
      return [
        '#markup' => '',
        '#cache' => [
          'max-age' => 0,
        ],
      ];
    }

    $output = [
      '#cache' => [
        'tags' => [
          static::LISTING_TAG,
        ],
      ],
    ];
    $enabled_solution_packs = islandora_solution_packs_get_required_objects();
    foreach ($enabled_solution_packs as $solution_pack_module => $solution_pack_info) {
      // @todo We should probably get the title of the solution pack from the
      // systems table for consistency in the interface.
      $solution_pack_name = $solution_pack_info['title'];
      $objects = array_filter($solution_pack_info['objects']);
      $class_name = SolutionPackForm::class;

      $output[$solution_pack_module] = $this->formBuilder()->getForm($class_name, $solution_pack_module, $solution_pack_name, $objects);

    }
    return $output;
  }

  /**
   * Page callback for the path "islandora".
   *
   * Redirects to the view of the object indicated by the Drupal variable
   * 'islandora_repository_pid'.
   */
  public function viewDefaultObject() {
    $pid = $this->config('islandora.settings')->get('islandora_repository_pid');
    return $this->redirect('islandora.view_object', ['object' => $pid]);
  }

  /**
   * Title callback for Drupal title to be the object label.
   */
  public function drupalTitle(AbstractObject $object) {
    module_load_include('inc', 'islandora', 'includes/breadcrumb');
    return $object->label;
  }

  /**
   * View islandora object.
   */
  public function viewObject(AbstractObject $object, Request $currentRequest) {
    module_load_include('inc', 'islandora', 'includes/breadcrumb');
    module_load_include('inc', 'islandora', 'includes/utilities');
    // XXX: This seems so very dumb but given how empty slugs don't play nice
    // in Drupal as defaults this needs to be the case. If it's possible to get
    // around this by making the empty slug route in YAML or a custom Routing
    // object we can remove this.
    if ($currentRequest->getRequestUri() === '/islandora/object/') {
      return $this->redirect('islandora.view_object', ['object' => $this->config('islandora.settings')->get('islandora_repository_pid')]);
    }
    // Warn if object is inactive or deleted.
    if ($object->state != 'A') {
      drupal_set_message($this->t('This object is not active. Metadata may not display correctly.'), 'warning');
    }
    // Optional pager parameters.
    $page_number = (empty($_GET['page'])) ? '1' : $_GET['page'];
    $page_size = (empty($_GET['pagesize'])) ? '10' : $_GET['pagesize'];
    $output = [];
    $hooks = islandora_build_hook_list(ISLANDORA_VIEW_HOOK, $object->models);
    foreach ($hooks as $hook) {
      // @todo Remove page number and size from this hook, implementers of the
      // hook should use drupal page handling directly.
      $temp = $this->moduleHandler()->invokeAll($hook, [
        $object,
        $page_number,
        $page_size,
      ]);
      if (!empty($temp)) {
        $output = array_merge_recursive($output, $temp);
      }
    }
    if (empty($output)) {
      // No results, use the default view.
      $output = islandora_default_islandora_view_object($object);
    }

    arsort($output);
    $this->moduleHandler()->alter($hooks, $object, $output);

    $this->renderer->addCacheableDependency($output, $object);

    return $output;
  }

  /**
   * Access callback for printing an object.
   */
  public function printObjectAccess($op, $object, AccountInterface $account) {
    $object = islandora_object_load($object);
    return AccessResult::allowedIf(islandora_print_object_access($op, $object, $account))
      ->addCacheableDependency($object)
      ->cachePerPermissions();
  }

  /**
   * Islandora printer object.
   */
  public function printerObject(AbstractObject $object) {
    $output = [];
    $temp_arr = [];

    $cache_meta = (new CacheableMetadata())
      ->addCacheableDependency($object);

    // Dispatch print hook.
    foreach (islandora_build_hook_list(ISLANDORA_PRINT_HOOK, $object->models) as $hook) {
      $temp = $this->moduleHandler()->invokeAll($hook, [$object]);
      if (!empty($temp)) {
        $temp_arr = array_merge_recursive($temp_arr, $temp);
      }
    }
    $cache_meta->applyTo($temp_arr);
    $output = islandora_default_islandora_printer_object($object, $this->renderer->render($temp_arr));
    arsort($output);

    // Prompt to print.
    $output['#attached']['library'][] = 'islandora/islandora-print-js';

    $cache_meta->applyTo($output);

    return $output;
  }

  /**
   * Renders the print page for the given object.
   *
   * Modules can either implement preprocess functions to append content onto
   * the 'content' variable, or override the display by providing a theme
   * suggestion.
   *
   * @param \AbstractObject $object
   *   The object.
   *
   * @return array
   *   A renderable array.
   */
  public function printObject(AbstractObject $object) {
    $output = [
      '#title' => $object->label,
      '#theme' => 'islandora_object_print',
      '#object' => $object,
    ];

    $this->renderer->addCacheableDependency($output, $object);

    return $output;
  }

  /**
   * Callback for an autocomplete field in the admin add datastream form.
   *
   * It lists the missing required (may be optional) datastreams.
   */
  public function addDatastreamFormAutocompleteCallback(AbstractObject $object, Request $request) {
    module_load_include('inc', 'islandora', 'includes/content_model');
    module_load_include('inc', 'islandora', 'includes/utilities');
    $query = $request->query->get('q');
    $dsids = array_keys(islandora_get_missing_datastreams_requirements($object));
    $query = trim($query);
    if (!empty($query)) {
      $filter = function ($id) use ($query) {
        return stripos($id, $query) !== FALSE;
      };
      $dsids = array_filter($dsids, $filter);
    }
    $output = [];
    foreach ($dsids as $dsid) {
      $output[] = ['value' => $dsid, 'label' => $dsid];
    }

    $response = new JsonResponse($output);

    $response->getCacheableMetadata()
      ->addCacheableDependency($object)
      ->addCacheContexts([
        'url.query_args:q',
      ]);

    return $response;
  }

  /**
   * Datastream title callback.
   */
  public function viewDatastreamTitle(AbstractDatastream $datastream, $download = FALSE, $version = NULL) {
    return $datastream->id;
  }

  /**
   * Callback function to view or download a datastream.
   *
   * @param \AbstractDatastream $datastream
   *   The datastream to view/download.
   * @param bool $download
   *   If TRUE the file is download to the user computer for viewing otherwise
   *   it will attempt to display in the browser natively.
   * @param int $version
   *   The version of the datastream to display.
   *
   * @return Symfony\Component\HttpFoundation\BinaryFileResponse|Symfony\Component\HttpFoundation\StreamedResponse
   *   A BinaryFileResponse if it's a ranged request, a StreamedResponse
   *   otherwise.
   */
  public function viewDatastream(AbstractDatastream $datastream, $download = FALSE, $version = NULL) {
    module_load_include('inc', 'islandora', 'includes/mimetype.utils');
    module_load_include('inc', 'islandora', 'includes/datastream');
    if ($version !== NULL) {
      if (isset($datastream[$version])) {
        $datastream = $datastream[$version];
      }
      else {
        throw new NotFoundHttpException();
      }
    }
    $headers = [
      'Content-type' => $datastream->mimetype,
      'Last-Modified' => $datastream->createdDate->format('D, d M Y H:i:s \G\M\T'),
    ];
    // XXX: The two response objects being used are considered non-cacheable by
    // default. By setting the cache control we allow these responses to be
    // cached. Non-cacheable responses wipe away certain headers that are nice
    // to have such as 'Last-Modified' and 'Etag' (for the checksum).
    $cache_control_visibility = $datastream->parent->repository->api->connection->username == 'anonymous' ? 'public' : 'private';
    $cache_control[] = $cache_control_visibility;
    $cache_control[] = 'must-revalidate';
    $cache_control[] = 'max-age=0';
    $headers['Cache-Control'] = implode(', ', $cache_control);
    if (isset($datastream->checksum)) {
      $headers['Etag'] = "\"{$datastream->checksum}\"";
    }
    $status = 200;
    if ($datastream->controlGroup == 'M' || $datastream->controlGroup == 'X') {
      $headers['Content-Length'] = $datastream->size;
    }
    $content_disposition = NULL;
    if ($download) {
      // Browsers will not append all extensions.
      $extension = '.' . islandora_get_extension_for_mimetype($datastream->mimetype);
      // Prevent adding on a duplicate extension.
      $label = $datastream->label;
      $extension_length = strlen($extension);
      $duplicate_extension_position = strlen($label) > $extension_length ?
        strripos($label, $extension, -$extension_length) :
        FALSE;
      $filename = $label;
      if ($duplicate_extension_position === FALSE) {
        $filename .= $extension;
      }

      // Allow other modules to modify or replace the filename.
      $this->moduleHandler()->alter('islandora_datastream_filename', $filename, $datastream);

      $content_disposition = "attachment; filename=\"{$filename}\"";
    }
    // We need to see if the chunking is being requested. This will mainly
    // happen with iOS video requests as they do not support any other way
    // to receive content for playback.
    if (isset($_SERVER['HTTP_RANGE'])) {
      // XXX: Can't make assertions on byte ranging of redirect datastreams.
      // @see https://jira.duraspace.org/browse/ISLANDORA-2084.
      if (!$download && $datastream->controlGroup == 'R') {
        $response = RedirectResponse::create($datastream->url);
      }
      else {
        module_load_include('inc', 'islandora', 'includes/datastream');
        $file_uri = islandora_view_datastream_retrieve_file_uri($datastream);
        $binary_content_disposition = isset($content_disposition) ? 'attachment' : NULL;
        $response = new BinaryFileResponse($file_uri, $status, $headers, $cache_control_visibility, $binary_content_disposition, FALSE, FALSE);
      }
    }
    else {
      $streaming_callback = function () use ($datastream) {
        $datastream->getContent('php://output');
      };
      if ($content_disposition) {
        $headers['Content-Disposition'] = $content_disposition;
      }
      $response = new StreamedResponse($streaming_callback, $status, $headers);
    }
    return $response;
  }

  /**
   * Callback to download the given datastream to the users computer.
   *
   * @param \AbstractDatastream $datastream
   *   The datastream to download.
   *
   * @return Symfony\Component\HttpFoundation\BinaryFileResponse|Symfony\Component\HttpFoundation\StreamedResponse
   *   A BinaryFileResponse if it's a ranged request, a StreamedResponse
   *   otherwise.
   */
  public function downloadDatastream(AbstractDatastream $datastream) {
    return $this->viewDatastream($datastream, TRUE);
  }

  /**
   * Page callback for editing a datastream.
   */
  public function editDatastream(AbstractDatastream $datastream) {
    module_load_include('inc', 'islandora', 'includes/utilities');

    $edit_registry = islandora_build_datastream_edit_registry($datastream);
    $edit_count = count($edit_registry);
    switch ($edit_count) {
      case 0:
        // No edit implementations.
        drupal_set_message($this->t('There are no edit methods specified for the @id datastream.', ['@id' => $datastream->id]));
        return $this->redirect('islandora.edit_object', ['object' => $datastream->parent->id]);

      case 1:
        // One registry implementation, go there.
        $entry = reset($edit_registry);
        return RedirectResponse::create($entry['url']);

      default:
        // Multiple edit routes registered.
        $list = [
          '#theme' => 'item_list',
          '#items' => [],
        ];
        foreach ($edit_registry as $entry) {
          $list['#items'][$entry['name']] = [
            '#type' => 'link',
            '#title' => $entry['name'],
            // XXX: Doesn't anything which accepts as a string... I foresee
            // having to rework the hook to return the route info (route name
            // and parameters).
            '#url' => Url::fromUserInput($entry['url']),
          ];
        }

        $this->renderer->addCacheableDependency($list, $datastream);

        return $list;
    }
  }

  /**
   * Page callback for the datastream version table.
   */
  public function datastreamVersionTable(AbstractDatastream $datastream) {
    module_load_include('inc', 'islandora', 'includes/datastream.version');
    return islandora_datastream_version_table($datastream);
  }

  /**
   * Page callback for session status messages.
   */
  public function eventStatus() {
    $results = FALSE;
    if (isset($_SESSION['islandora_event_messages'])) {
      foreach ($_SESSION['islandora_event_messages'] as $message) {
        drupal_set_message($message['message'], $message['severity']);
        $results = TRUE;
      }
      unset($_SESSION['islandora_event_messages']);
    }
    $text = ($results) ? $this->t('The status messages above will be deleted after viewing this page.') : $this->t('No messages to display.');
    return ['#markup' => $text];
  }

  /**
   * Autocomplete the content model name.
   */
  public function contentModelAutocomplete(Request $request) {
    module_load_include('inc', 'islandora', 'includes/content_model.autocomplete');
    $string = $request->query->get('q');
    $content_models = islandora_get_content_model_names();
    $output = [];
    foreach ($content_models as $model => $label) {
      if (preg_match("/{$string}/i", $label) !== 0) {
        $output[] = ['value' => $model, 'label' => $label];
      }
    }
    $response = new JsonResponse($output);

    $response->getCacheableMetadata()
      ->addCacheableDependency($this->config('islandora.settings'))
      ->addCacheTags([static::LISTING_TAG])
      ->addCacheContexts([
        'url.query_args:q',
      ]);

    return $response;
  }

}
